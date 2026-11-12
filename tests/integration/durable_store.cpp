#include <capstan/durable/sqlite_store.hpp>
#include "tests/check.hpp"
#include <cstdio>
#include <sqlite3.h>
#include <cstring>
#include <stdexcept>
using namespace capstan;
using namespace capstan::durable;
namespace {
std::string temporary() {char path[]="/tmp/capstan-durable-XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);close(fd);return path;}
fault_point crash_at;
void crash(fault_point point) {if(point==crash_at) _exit(87);}
bytes read(sqlite_store& store,const char* key) {bytes b;bool found=false;CHECK(store.get(key,b,found)&&found);return b;}
}
int main() {
    for(auto point:{fault_point::before_commit,fault_point::after_commit}) {
        auto path=temporary();
        {sqlite_store db(path);CHECK(db.commit({{"data",{1}},{"receipt",{1}}}).outcome==commit_outcome::committed);}
        auto pid=fork();CHECK(pid>=0);
        if(pid==0) {sqlite_store db(path);crash_at=point;db.commit({{"data",{2}},{"receipt",{2}}},crash);_exit(99);}
        int state;CHECK(waitpid(pid,&state,0)==pid && WIFEXITED(state) && WEXITSTATUS(state)==87);
        {sqlite_store db(path);auto data=read(db,"data"),receipt=read(db,"receipt");
            CHECK(data==receipt && data==bytes{static_cast<std::uint8_t>(point==fault_point::before_commit ? 1:2)});
            CHECK(db.claim_actor("host") && !db.claim_actor("host"));
            sqlite_store other(path);CHECK(!other.claim_actor("host") && other.claim_actor("table"));
            const std::vector<record> empty_batch{{"empty",{}}};
            CHECK(db.commit(empty_batch).outcome==commit_outcome::committed && read(db,"empty").empty());
            std::vector<record> rows;CHECK(db.list("rece",rows) && rows.size()==1 && rows[0].key=="receipt");
        }
        std::remove(path.c_str());std::remove((path+".host.lock").c_str());std::remove((path+".table.lock").c_str());
    }
    // Convert the old WITHOUT ROWID layout atomically, retaining all values
    // (including empty BLOBs) and leaving old SQL clients able to read records.
    for(auto point:{fault_point::before_layout_commit,fault_point::after_layout_commit}) {
        auto path=temporary();sqlite3* db=nullptr;CHECK(sqlite3_open(path.c_str(),&db)==SQLITE_OK);
        CHECK(sqlite3_exec(db,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE capstan_records(key TEXT PRIMARY KEY NOT NULL,value BLOB NOT NULL) WITHOUT ROWID; INSERT INTO capstan_records VALUES('base',zeroblob(8388608)),('receipt',X'0102'),('empty',X'');",nullptr,nullptr,nullptr)==SQLITE_OK);
        CHECK(sqlite3_close(db)==SQLITE_OK);
        auto pid=fork();CHECK(pid>=0);
        if(pid==0) {crash_at=point;sqlite_store converted(path,crash);_exit(99);}
        int state;CHECK(waitpid(pid,&state,0)==pid&&WIFEXITED(state)&&WEXITSTATUS(state)==87);
        CHECK(sqlite3_open(path.c_str(),&db)==SQLITE_OK);
        sqlite3_stmt* schema=nullptr;const int probe=sqlite3_prepare_v2(db,"SELECT rowid FROM capstan_records LIMIT 0",-1,&schema,nullptr);
        CHECK((probe==SQLITE_OK)==(point==fault_point::after_layout_commit));sqlite3_finalize(schema);
        // Keep a legacy prepared statement across the successful migration.
        sqlite3_stmt* legacy_read=nullptr;CHECK(sqlite3_prepare_v2(db,"SELECT value FROM capstan_records WHERE key='receipt'",-1,&legacy_read,nullptr)==SQLITE_OK);
        sqlite3_stmt* legacy_put=nullptr;CHECK(sqlite3_prepare_v2(db,"INSERT INTO capstan_records(key,value) VALUES('legacy-cache',X'0405') ON CONFLICT(key) DO UPDATE SET value=excluded.value",-1,&legacy_put,nullptr)==SQLITE_OK);
        {
            sqlite_store converted(path);
            auto base=read(converted,"base");CHECK(base==bytes(8388608,0));
            CHECK(read(converted,"receipt")==bytes({1,2})&&read(converted,"empty").empty());
            for(unsigned n=0;n<64;++n) {
                const bytes expected{static_cast<std::uint8_t>(n)};
                CHECK(converted.commit({{"receipt",expected},{"next",expected}}).outcome==commit_outcome::committed);
                CHECK(read(converted,"receipt")==expected&&read(converted,"next")==expected);
                bytes missing;bool found=true;CHECK(converted.get("missing",missing,found)&&!found);
            }
            CHECK(sqlite3_step(legacy_read)==SQLITE_ROW&&sqlite3_column_bytes(legacy_read,0)==1);
            CHECK(static_cast<const unsigned char*>(sqlite3_column_blob(legacy_read,0))[0]==63);
            sqlite3_reset(legacy_read);
            CHECK(sqlite3_step(legacy_put)==SQLITE_DONE);sqlite3_reset(legacy_put);
            CHECK(read(converted,"legacy-cache")==bytes({4,5}));
        }
        sqlite3_finalize(legacy_read);sqlite3_finalize(legacy_put);CHECK(sqlite3_close(db)==SQLITE_OK);
        {sqlite_store reopened(path);CHECK(read(reopened,"receipt")==bytes{63});}
        std::remove(path.c_str());
    }
#if CAPSTAN_HAS_EXCEPTIONS
    // Unknown schema or user-added indexes/triggers must never be silently
    // dropped by the owned-layout migration. Fail before touching records.
    for(unsigned variant=0;variant<3;++variant) {
        auto path=temporary();sqlite3* db=nullptr;CHECK(sqlite3_open(path.c_str(),&db)==SQLITE_OK);
        const std::string schema=variant==0 ?
            "CREATE TABLE capstan_records(key TEXT PRIMARY KEY NOT NULL,value BLOB NOT NULL,extra INTEGER DEFAULT 7) WITHOUT ROWID" :
            "CREATE TABLE capstan_records(key TEXT PRIMARY KEY NOT NULL,value BLOB NOT NULL) WITHOUT ROWID";
        CHECK(sqlite3_exec(db,schema.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);
        CHECK(sqlite3_exec(db,"INSERT INTO capstan_records(key,value) VALUES('saved',X'09')",nullptr,nullptr,nullptr)==SQLITE_OK);
        if(variant==1)CHECK(sqlite3_exec(db,"CREATE INDEX user_index ON capstan_records(value)",nullptr,nullptr,nullptr)==SQLITE_OK);
        if(variant==2)CHECK(sqlite3_exec(db,"CREATE TRIGGER user_trigger BEFORE DELETE ON capstan_records BEGIN SELECT RAISE(ABORT,'keep'); END",nullptr,nullptr,nullptr)==SQLITE_OK);
        CHECK(sqlite3_close(db)==SQLITE_OK);
        bool rejected=false;try {sqlite_store unsupported(path);} catch(const std::runtime_error&) {rejected=true;}CHECK(rejected);
        CHECK(sqlite3_open(path.c_str(),&db)==SQLITE_OK);sqlite3_stmt* query=nullptr;
        CHECK(sqlite3_prepare_v2(db,"SELECT sql FROM sqlite_master WHERE name='capstan_records'",-1,&query,nullptr)==SQLITE_OK&&sqlite3_step(query)==SQLITE_ROW);
        CHECK(schema==reinterpret_cast<const char*>(sqlite3_column_text(query,0)));sqlite3_finalize(query);
        CHECK(sqlite3_prepare_v2(db,"SELECT value FROM capstan_records WHERE key='saved'",-1,&query,nullptr)==SQLITE_OK&&sqlite3_step(query)==SQLITE_ROW);
        CHECK(sqlite3_column_bytes(query,0)==1&&static_cast<const unsigned char*>(sqlite3_column_blob(query,0))[0]==9);sqlite3_finalize(query);
        CHECK(sqlite3_close(db)==SQLITE_OK);std::remove(path.c_str());
    }
#endif
    std::cout<<"SQLite atomic data/receipt crash boundaries and exclusive actor claims passed\n";
}
