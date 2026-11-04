#include <capstan/error_policy.hpp>
#include "common.hpp"
#include <capstan/access/capability.hpp>
#include <chrono>
using namespace capstan;
int main(int argc,char** argv) {
    CAPSTAN_TRY {
        auto args=app::options(argc,argv);auto n=app::option_number(args,"--concurrency",1);if(!n || n>128) app::startup_failure("concurrency 1..128");
        bool raw_writes=app::option_number(args,"--write",0)!=0;
        auto credential=app::load_credential(app::required(args,"--credential"));auto* loop=g_main_loop_new(nullptr,FALSE);auto* ctx=g_main_loop_get_context(loop);
        gio_client table_socket(ctx,"127.0.0.1",app::port_option(args,"--table-port",0));app::authenticated_endpoint table(table_socket,credential);
        std::vector<std::unique_ptr<gio_client>> sockets;
        std::vector<std::unique_ptr<app::authenticated_endpoint>> endpoints;
        for(id i=0;i<n;++i) {
            sockets.emplace_back(new gio_client(ctx,"127.0.0.1",app::port_option(args,"--host-port",0)));
            endpoints.emplace_back(new app::authenticated_endpoint(*sockets.back(),credential));
        }
        std::unique_ptr<read_capability> capability;
        std::vector<id> latencies;
        id done_count=0,ok=0,not_submitted=0,unknown=0,rejected=0;
        auto start=std::chrono::steady_clock::now();
        start_io(table,wire::command(wire::fetch_grant),[&](io_result io) {
            auto r=wire::decode_reply(io.payload);wire::require(r.code==status::ok && r.values.size()==1);
            auto grant=wire::decode_grant(wire::string(r.values[0]));
            if(!raw_writes) capability.reset(new read_capability(grant));
            id request_id=id{1}<<61;
            start=std::chrono::steady_clock::now();
            // Two attempts per connection deliberately exercise the single
            // outstanding request bound. Read borrows share one local lineage.
            for(auto& ep:endpoints) for(unsigned attempt=0;attempt<2;++attempt) {
                auto submitted=std::chrono::steady_clock::now();
                auto completion=std::function<void(wire::reply)>([&,submitted](wire::reply reply) {
                    latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-submitted).count());
                    if(reply.state==outcome::not_submitted) ++not_submitted;
                    else if(reply.state==outcome::unknown) ++unknown;
                    else if(reply.code==status::ok) ++ok;
                    else ++rejected;
                    if(++done_count==2*n) g_main_loop_quit(loop);
                });
                if(raw_writes) {
                    auto packet=wire::encode_call({grant,request_id++,method::add_range},{wire::u(0),wire::u(1),wire::u(1)});
                    start_io(*ep,std::move(packet),[completion=std::move(completion)](io_result io) mutable {
                        if(io.state==outcome::completed) completion(wire::decode_reply(io.payload));
                        else completion({io.state,status::invalid,{}});
                    });
                } else start_call(*ep,capability->version(),std::move(completion));
            }
        });
        g_main_loop_run(loop);
        auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        std::cout<<"LOAD concurrency="<<n<<" attempts="<<done_count<<" ok="<<ok<<" not_submitted="<<not_submitted<<" unknown="<<unknown<<" rejected="<<rejected<<" wall_ns="<<elapsed<<'\n';
        for(auto ns:latencies) std::cout<<"SAMPLE ns="<<ns<<'\n';
        g_main_loop_unref(loop);
        return done_count==2*n && not_submitted+unknown>0 ? 0:2;
    } CAPSTAN_CATCH_STD(e){std::cerr<<e.what()<<'\n';return 1;}
}
