#include <capstan/durable/sqlite_store.hpp>
#include <capstan/error_policy.hpp>
#include <sqlite3.h>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <stdexcept>
namespace capstan { namespace durable {
namespace {
struct statement {
    sqlite3_stmt* value=nullptr;
    ~statement() noexcept {if(value) sqlite3_finalize(value);}
};
// Cached plans never retain caller data or an active read across operations.
struct reset_statement {
    sqlite3_stmt* value;
    ~reset_statement() noexcept {if(value) {sqlite3_reset(value);sqlite3_clear_bindings(value);}}
};
int text(sqlite3_stmt* s,int n,const std::string& value) noexcept {
    if(value.size()>INT_MAX) return SQLITE_TOOBIG;
    return sqlite3_bind_text(s,n,value.data(),static_cast<int>(value.size()),SQLITE_TRANSIENT);
}
}
bool sqlite_store::exec(const char* sql) noexcept {error_=sqlite3_exec(db_,sql,nullptr,nullptr,nullptr);return error_==SQLITE_OK;}
sqlite_store::sqlite_store(const std::string& path,fault_hook layout_hook) {
    // Refuse in-memory/URI databases: this class promises on-disk commits.
    if(path.empty() || path==":memory:" || path.find("file:")==0)
        CAPSTAN_RAISE(std::invalid_argument("persistent SQLite needs a local file path"));
    const int fd=::open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if(fd<0) CAPSTAN_RAISE(std::runtime_error("cannot open persistent database file"));
    ::close(fd);
    char* canonical=::realpath(path.c_str(),nullptr);
    if(!canonical) CAPSTAN_RAISE(std::runtime_error("cannot resolve persistent database path"));
    path_=canonical;std::free(canonical);
    auto fail=[&] {if(db_) {sqlite3_close_v2(db_);db_=nullptr;} CAPSTAN_RAISE(std::runtime_error("cannot initialize durable SQLite store"));};
    if(sqlite3_open_v2(path_.c_str(),&db_,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) {fail();return;}
    sqlite3_busy_timeout(db_,5000);
    if(!exec("PRAGMA journal_mode=WAL") || !exec("PRAGMA synchronous=FULL")) {fail();return;}
#ifdef __APPLE__
    if(!exec("PRAGMA fullfsync=ON")) {fail();return;}
#endif
    { statement mode,sync;
    if(sqlite3_prepare_v2(db_,"PRAGMA journal_mode",-1,&mode.value,nullptr)!=SQLITE_OK ||
       sqlite3_step(mode.value)!=SQLITE_ROW ||
       std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(mode.value,0)),"wal") ||
       sqlite3_prepare_v2(db_,"PRAGMA synchronous",-1,&sync.value,nullptr)!=SQLITE_OK ||
       sqlite3_step(sync.value)!=SQLITE_ROW || sqlite3_column_int(sync.value,0)!=2) {fail();return;} }
    // WITHOUT ROWID places large immutable base BLOBs inside the key tree.
    // Small-key searches/rebalances can then pull their overflow pages. Keep a
    // narrow key->rowid index and the values in the rowid table instead.
    // The layout conversion is one local transaction; the logical records and
    // application envelopes are unchanged. Recheck after taking the writer lock.
    if(!exec("BEGIN IMMEDIATE")) {fail();return;}
    bool changed=false;
    bool layout_ok=false;
    CAPSTAN_TRY { layout_ok=[&]() -> bool {
        if(!exec("CREATE TABLE IF NOT EXISTS capstan_records(key TEXT PRIMARY KEY NOT NULL,value BLOB NOT NULL)")) {return false;}
        {
            statement probe;
            const int rc=sqlite3_prepare_v2(db_,"SELECT rowid FROM capstan_records LIMIT 0",-1,&probe.value,nullptr);
            if(rc!=SQLITE_OK) {
                // Only the known legacy WITHOUT ROWID layout may be rebuilt.
                statement schema;
                if(rc!=SQLITE_ERROR || sqlite3_prepare_v2(db_,"SELECT sql FROM sqlite_master WHERE type='table' AND name='capstan_records'",-1,&schema.value,nullptr)!=SQLITE_OK ||
                   sqlite3_step(schema.value)!=SQLITE_ROW || !sqlite3_column_text(schema.value,0) ||
                   std::strcmp(reinterpret_cast<const char*>(sqlite3_column_text(schema.value,0)),
                       "CREATE TABLE capstan_records(key TEXT PRIMARY KEY NOT NULL,value BLOB NOT NULL) WITHOUT ROWID")) {return false;}
                statement extras;
                if(sqlite3_prepare_v2(db_,"SELECT count(*) FROM sqlite_master WHERE tbl_name='capstan_records' AND type IN ('index','trigger') AND sql IS NOT NULL",-1,&extras.value,nullptr)!=SQLITE_OK ||
                   sqlite3_step(extras.value)!=SQLITE_ROW || sqlite3_column_int(extras.value,0)!=0) return false;
                changed=true;
            }
        }
        if(changed && (!exec("CREATE TABLE capstan_records_rowid_upgrade(key TEXT PRIMARY KEY NOT NULL,value BLOB NOT NULL)") ||
           !exec("INSERT INTO capstan_records_rowid_upgrade SELECT key,value FROM capstan_records") ||
           !exec("DROP TABLE capstan_records") ||
           !exec("ALTER TABLE capstan_records_rowid_upgrade RENAME TO capstan_records"))) {return false;}
        if(changed && layout_hook) layout_hook(fault_point::before_layout_commit);
        if(!exec("COMMIT")) {return false;}
        if(changed && layout_hook) layout_hook(fault_point::after_layout_commit);
        return true;
    }(); } CAPSTAN_CATCH_ALL {layout_ok=false;}
    if(!layout_ok) {sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);fail();return;}
}
sqlite_store::~sqlite_store() noexcept {
    if(get_statement_) sqlite3_finalize(get_statement_);
    if(put_statement_) sqlite3_finalize(put_statement_);
    if(db_) sqlite3_close(db_);
    for(int fd:locks_) ::close(fd);
}
bool sqlite_store::healthy() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return !failed_;}
int sqlite_store::error() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return error_;}
bool sqlite_store::claim_actor(const std::string& actor) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    CAPSTAN_TRY {
        if(actor.empty() || actor.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos) return false;
        for(const auto& prior:actors_) if(prior==actor) return false;
        auto name=path_+"."+actor+".lock";
        int fd=::open(name.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
        if(fd<0) return false;
        if(::flock(fd,LOCK_EX|LOCK_NB)) {::close(fd);return false;}
        CAPSTAN_TRY {actors_.reserve(actors_.size()+1);locks_.reserve(locks_.size()+1);actors_.push_back(actor);locks_.push_back(fd);}
        CAPSTAN_CATCH_ALL {::close(fd);return false;}
        return true;
    } CAPSTAN_CATCH_ALL {return false;}
}
bool sqlite_store::get(const std::string& key,bytes& value,bool& found) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(failed_) return false;
    CAPSTAN_TRY {
        if(!get_statement_) error_=sqlite3_prepare_v2(db_,"SELECT value FROM capstan_records WHERE key=?1",-1,&get_statement_,nullptr);
        else error_=SQLITE_OK;
        reset_statement s{get_statement_};
        if(error_==SQLITE_OK) error_=text(s.value,1,key);
        if(error_!=SQLITE_OK) {failed_=true;return false;}
        error_=sqlite3_step(s.value);
        if(error_==SQLITE_DONE) {error_=SQLITE_OK;found=false;return true;}
        if(error_!=SQLITE_ROW) {failed_=true;return false;}
        const auto size=sqlite3_column_bytes(s.value,0);
        const auto* data=static_cast<const std::uint8_t*>(sqlite3_column_blob(s.value,0));
        bytes staged(static_cast<std::size_t>(size));if(size) std::memcpy(staged.data(),data,size);
        value=std::move(staged);error_=SQLITE_OK;found=true;return true;
    } CAPSTAN_CATCH_ALL {failed_=true;error_=SQLITE_NOMEM;return false;}
}
bool sqlite_store::list(const std::string& prefix,std::vector<record>& rows,std::size_t limit) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(failed_) return false;
    CAPSTAN_TRY {
        statement s;
        error_=sqlite3_prepare_v2(db_,"SELECT key,value FROM capstan_records WHERE substr(key,1,length(?1))=?1 ORDER BY key",-1,&s.value,nullptr);
        if(error_==SQLITE_OK) error_=text(s.value,1,prefix);
        if(error_!=SQLITE_OK) {failed_=true;return false;}
        std::vector<record> staged;
        while((error_=sqlite3_step(s.value))==SQLITE_ROW) {
            if(staged.size()>=limit) {failed_=true;error_=SQLITE_TOOBIG;return false;}
            record row;row.key.assign(reinterpret_cast<const char*>(sqlite3_column_text(s.value,0)),static_cast<std::size_t>(sqlite3_column_bytes(s.value,0)));
            const auto size=sqlite3_column_bytes(s.value,1);
            row.value.resize(static_cast<std::size_t>(size));
            if(size) std::memcpy(row.value.data(),sqlite3_column_blob(s.value,1),size);
            staged.emplace_back(std::move(row));
        }
        if(error_!=SQLITE_DONE) {failed_=true;return false;}
        rows=std::move(staged);error_=SQLITE_OK;return true;
    } CAPSTAN_CATCH_ALL {failed_=true;error_=SQLITE_NOMEM;return false;}
}
commit_result sqlite_store::commit(const std::vector<record>& batch,fault_hook hook) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if(failed_) return {commit_outcome::unknown,error_};
    bool committing=false;
    CAPSTAN_TRY {
        if(!exec("BEGIN IMMEDIATE")) {failed_=true;return {commit_outcome::not_committed,error_};}
        if(!put_statement_) error_=sqlite3_prepare_v2(db_,"INSERT INTO capstan_records(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value",-1,&put_statement_,nullptr);
        else error_=SQLITE_OK;
        reset_statement s{put_statement_};
        if(error_==SQLITE_OK) for(const auto& row:batch) {
            sqlite3_reset(s.value);sqlite3_clear_bindings(s.value);
            error_=text(s.value,1,row.key);
            if(error_==SQLITE_OK) error_=row.value.size()>INT_MAX ? SQLITE_TOOBIG :
                sqlite3_bind_blob(s.value,2,row.value.empty() ? static_cast<const void*>("") : row.value.data(),static_cast<int>(row.value.size()),SQLITE_TRANSIENT);
            if(error_==SQLITE_OK) error_=sqlite3_step(s.value);
            if(error_!=SQLITE_DONE) break;
            error_=SQLITE_OK;
        }
        if(error_!=SQLITE_OK) {
            const int error=error_;sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);failed_=true;
            return {commit_outcome::not_committed,error};
        }
        if(hook) hook(fault_point::before_commit);
        committing=true;
        if(!exec("COMMIT")) {
            const int error=error_;sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);failed_=true;
            return {commit_outcome::unknown,error};
        }
        if(hook) hook(fault_point::after_commit);
        return {commit_outcome::committed,SQLITE_OK};
    } CAPSTAN_CATCH_ALL {
        sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);failed_=true;error_=SQLITE_NOMEM;
        return {committing ? commit_outcome::unknown : commit_outcome::not_committed,SQLITE_NOMEM};
    }
}
}}
