#define CAPSTAN_SQLITE_TABLE
#define CAPSTAN_TABLE_NO_MAIN
#include "src/apps/table.cpp"
#include "examples/owner/sqlite.hpp"
#include "src/apps/owner_host.hpp"
#include "tests/check.hpp"
#include <cstdio>
using runtime_type=basic_host_runtime<examples::sqlite_owner_backend>;
namespace {
app::config config() {app::config c{{1,2,3},4,{}};for(unsigned i=0;i<3;++i)c.credentials[i][0]=10+i;return c;}
grant source() {grant g;g.key=config().key;g.epoch=1;g.holder=1;g.grant_id=11;g.mode=access_mode::exclusive;g.secret[0]=21;return g;}
grant target() {auto g=source();g.epoch=2;g.holder=2;g.grant_id=12;g.secret[0]=22;return g;}
std::string temporary() {char path[]="/tmp/capstan-table-XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);close(fd);return path;}
unsigned commits=0,crash_commit=0;
durable::fault_point crash_at;
void crash(durable::fault_point point) {
    if(point==durable::fault_point::before_commit) ++commits;
    if(commits==crash_commit && point==crash_at) _exit(87);
}
void pump(GMainContext* c,const std::function<bool()>& done) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!done()) {while(g_main_context_iteration(c,FALSE)) {} CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(std::chrono::microseconds(100));}
}
}
int main() {
    // Crash before/after the intent transaction, and before/after confirmation.
    // In the latter cases Host has installed epoch 2 while Table dies. Both
    // processes terminate together; fresh instances recover from their files.
    for(unsigned boundary:{1u,2u}) for(auto point:{durable::fault_point::before_commit,durable::fault_point::after_commit}) {
        auto host_path=temporary(),table_path=temporary();
        auto* c=g_main_context_new();
        {auto b=std::make_shared<examples::sqlite_owner_backend>(host_path);runtime_type host(config(),c,1024,0,16,b);host.trace_requests=false;
            CHECK(host.authority.set_authority({config().key,config().control,10,0,1,false,{source()}})==status::ok && host.collect_resource());
        }g_main_context_unref(c);
        auto pid=fork();CHECK(pid>=0);
        if(pid==0) {
            auto* context=g_main_context_new();auto b=std::make_shared<examples::sqlite_owner_backend>(host_path);
            runtime_type host(config(),context,1024,0,16,b);host.trace_requests=false;
            gio_server server(context,0,[&](id n,bytes packet,io_completion done){host.handle(n,std::move(packet),std::move(done));});CHECK(server.valid());
            table_runtime table(config(),context,server.port(),table_path);
            auto op=std::make_shared<table_runtime::transition_operation>();op->command={1,1};op->started=true;
            CHECK(table.directory.begin_at(1,{target()},false,op->target)==status::ok);
            table.history.emplace(op->command,table_runtime::command_record{{1,2,3},{}});table.changing=true;
            op->done=[](io_result){_exit(93);};
            commits=0;crash_commit=boundary;crash_at=point;table.fault=crash;
            table.advance(op);pump(context,[]{return false;});_exit(99);
        }
        int state;CHECK(waitpid(pid,&state,0)==pid && WIFEXITED(state) && WEXITSTATUS(state)==87);
        c=g_main_context_new();
        {auto b=std::make_shared<examples::sqlite_owner_backend>(host_path);runtime_type host(config(),c,1024,0,16,b);host.trace_requests=false;
            gio_server server(c,0,[&](id n,bytes packet,io_completion done){host.handle(n,std::move(packet),std::move(done));});CHECK(server.valid());
            table_runtime table(config(),c,server.port(),table_path);
            if(boundary==1 && point==durable::fault_point::before_commit) {
                CHECK(!table.changing && table.history.empty() && host.authority.inspect().epoch==1);
            } else if(boundary==2 && point==durable::fault_point::after_commit) {
                CHECK(!table.changing && table.directory.epoch()==2 && host.authority.inspect().grants==std::vector<grant>{target()});
                CHECK(wire::decode_reply(table.history.at({1,1}).response).code==status::ok);
                CHECK(!table.uncertain_operation);
            } else {
                CHECK(table.changing && table.uncertain_operation && table.uncertain_operation->ever_uncertain);
                auto op=table.uncertain_operation;CHECK(op->target.grants==std::vector<grant>{target()} && op->target.tx==1);
                // A failed retry cannot prove that the old attempt was unsent.
                bool pending=false;op->done=[&](io_result io){CHECK(wire::decode_reply(io.payload).state==outcome::unknown);pending=true;};
                table.on_ack(op,{outcome::not_submitted,{}});CHECK(pending && table.changing);
                bool completed=false;
                op->done=[&](io_result io){auto r=wire::decode_reply(io.payload);CHECK(r.state==outcome::completed && r.code==status::ok);completed=true;};
                table.advance(op);pump(c,[&]{return completed;});
                CHECK(!table.changing && table.directory.epoch()==2 && host.authority.inspect().grants==std::vector<grant>{target()});
                transition next;CHECK(table.directory.begin({source()},false,next)==status::ok && next.tx==2); // Sequence never reused.
            }
        }
        while(g_main_context_iteration(c,FALSE)) {}g_main_context_unref(c);
        std::remove(host_path.c_str());std::remove(table_path.c_str());
        std::remove((host_path+".host.lock").c_str());std::remove((table_path+".table.lock").c_str());
    }
    std::cout<<"Table durable intent/confirmation crash boundaries, exact RPC retry and restored sequence passed\n";
}
