#include <capstan/durable/sqlite_store.hpp>
#include "tests/check.hpp"
#include <cstdio>
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
    std::cout<<"SQLite atomic data/receipt crash boundaries and exclusive actor claims passed\n";
}
