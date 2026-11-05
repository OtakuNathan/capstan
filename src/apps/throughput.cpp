// Native bounded-batch driver; one in-flight authenticated RPC per connection.
#include "common.hpp"
#include <capstan/access/capability.hpp>
#include <chrono>
#include <fstream>
#include <limits>
#include <vector>
using namespace capstan;
using clock_type = std::chrono::steady_clock;
struct sample { id sequence,slot,ns; outcome state; status code; bool valid; };
struct load {
    GMainLoop* loop=g_main_loop_new(nullptr,FALSE);
    GMainContext* context=g_main_loop_get_context(loop);
    std::vector<std::unique_ptr<gio_client>> sockets;
    std::vector<std::unique_ptr<app::authenticated_endpoint>> endpoints;
    std::unique_ptr<read_capability> capability;
    std::vector<sample> rows;
    id concurrency, count, warmup, issued=0, completed=0, target=0, request_base;
    bool measuring=false, scan;
    clock_type::time_point began;
    id elapsed=0,warm_errors=0;
    summary_t expected;
    load(id n,id c,id w,id base,bool s,summary_t e):concurrency(n),count(c),warmup(w),request_base(base),scan(s),expected(e) {rows.reserve(c);}
    ~load() {endpoints.clear();sockets.clear();g_main_loop_unref(loop);}
    void phase(bool measured) {
        measuring=measured;issued=completed=0;target=measured ? count:warmup;
        if(!target) {phase(true);return;}
        began=clock_type::now();
        for(id i=0;i<concurrency && issued<target;++i) issue(i);
    }
    void issue(id slot) {
        if(issued>=target) return;
        submit(slot,issued++);
    }
    void submit(id slot,id sequence) {
        auto started=clock_type::now();
        std::function<void(wire::reply)> done=[this,slot,sequence,started](wire::reply r) {
            bool valid=false;
            if(scan) {summary_t result;valid=typed_call<summary_t>::try_decode(r,result) && result==expected;}
            else {id result=99;valid=typed_call<id>::try_decode(r,result) && result==0;}
            auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now()-started).count();
            if(measuring) rows.push_back({sequence,slot,static_cast<id>(ns),r.state,r.code,valid});
            else if(!valid) ++warm_errors;
            if(++completed==target) {
                elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now()-began).count();
                if(measuring) g_main_loop_quit(loop);
                else app::post(context,[this]{phase(true);});
            } else if(issued<target) {
                // Post refill so synchronous submission failures cannot recurse.
                // Reserve the request now: completing the final request then
                // also proves no deferred refill still refers to this object.
                const auto next=issued++;
                app::post(context,[this,slot,next]{submit(slot,next);});
            }
        };
        if(scan) {auto call=capability->summary();call.request_id+=request_base;start_call(*endpoints[slot],std::move(call),std::move(done));}
        else {auto call=capability->version();call.request_id+=request_base;start_call(*endpoints[slot],std::move(call),std::move(done));}
    }
};
int main(int argc,char** argv) {
    auto args=app::options(argc,argv);
    auto n=app::option_number(args,"--concurrency",1),count=app::option_number(args,"--count",3000),warmup=app::option_number(args,"--warmup",128);
    if(!n || n>80 || !count || count>1048576 || warmup>65536) app::startup_failure("invalid bounds");
    const auto workload=app::required(args,"--workload");
    if(workload!="version" && workload!="summary") app::startup_failure("workload must be version or summary");
    const bool scan=workload=="summary";
    summary_t expected{app::option_number(args,"--expected-count",0),app::option_number(args,"--expected-sum",0),app::option_number(args,"--expected-checksum",0),0};
    if(scan && !expected.count) app::startup_failure("summary requires expected count/sum/checksum");
    const auto request_base=app::option_number(args,"--request-base",id{1}<<32);
    if(request_base>std::numeric_limits<id>::max()-count-warmup) app::startup_failure("request-id range overflow");
    load job(n,count,warmup,request_base,scan,expected);
    auto credential=app::load_credential(app::required(args,"--credential"));
    gio_client table_socket(job.context,"127.0.0.1",app::port_option(args,"--table-port",0));
    app::authenticated_endpoint table(table_socket,credential);
    for(id i=0;i<n;++i) {
        job.sockets.emplace_back(new gio_client(job.context,"127.0.0.1",app::port_option(args,"--host-port",0)));
        job.endpoints.emplace_back(new app::authenticated_endpoint(*job.sockets.back(),credential));
    }
    auto* timeout=g_timeout_source_new_seconds(45);
    g_source_set_callback(timeout,[](gpointer)->gboolean {app::startup_failure("load watchdog expired");},nullptr,nullptr);
    g_source_attach(timeout,job.context);
    start_io(table,wire::command(wire::fetch_grant),[&](io_result io) {
        wire::reply r;grant g;
        if(io.state!=outcome::completed || !wire::try_decode_reply(io.payload,r) || r.state!=outcome::completed || r.code!=status::ok || r.values.size()!=1 || !wire::is_string(r.values[0]) || !wire::try_decode_grant(wire::string(r.values[0]),g)) app::startup_failure("grant fetch failed");
        job.capability.reset(new read_capability(g));job.phase(false);
    });
    g_main_loop_run(job.loop);g_source_destroy(timeout);g_source_unref(timeout);
    std::ofstream output(app::required(args,"--output"));
    output<<"sequence,slot,ns,outcome,status,valid\n";
    id ok=0;
    for(auto& r:job.rows) {output<<r.sequence<<','<<r.slot<<','<<r.ns<<','<<static_cast<unsigned>(r.state)<<','<<status_name(r.code)<<','<<r.valid<<'\n';ok+=r.valid;}
    output.close();if(!output) app::startup_failure("sample output failed");
    std::cout<<"LOAD concurrency="<<n<<" attempts="<<job.rows.size()<<" ok="<<ok<<" errors="<<job.rows.size()-ok<<" warm_errors="<<job.warm_errors<<" wall_ns="<<job.elapsed<<'\n'<<std::flush;
    return job.rows.size()==count && ok==count && !job.warm_errors ? 0:2;
}
