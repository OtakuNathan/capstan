// Trusted-lab test client. Uses the actual authenticated codec + GIO + FF path.
#include "src/apps/common.hpp"
#include "tests/check.hpp"
using namespace capstan;
int main(int argc,char** argv) {
    CAPSTAN_TRY {
        auto args=app::options(argc,argv);auto config=app::load_config(args.at("--config"));
        auto* context=g_main_context_default();auto* loop=g_main_loop_new(context,FALSE);
        gio_client socket(context,"127.0.0.1",app::port_option(args,"--host-port",0));
        app::authenticated_endpoint host(socket,config.credentials[2]);
        id tx=app::option_number(args,"--tx",1000);
        bytes packet;
        if(args.at("--operation")=="query") packet=wire::command(wire::query_ack,{wire::u(tx)});
        else {
            id epoch=app::option_number(args,"--epoch",1);
            transition t{config.key,config.control,tx,epoch,epoch+1,
                app::option_number(args,"--destroy",0)!=0,{}};
            packet=wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition(t)))});
        }
        bool finished=false;int code=1;
        start_io(host,std::move(packet),[&](io_result result){
            CAPSTAN_TRY {
                CHECK(result.state==outcome::completed);
                auto reply=wire::decode_reply(result.payload);
                std::cout<<"CONTROL status="<<status_name(reply.code);
                if(reply.code==status::ok) {
                    CHECK(reply.values.size()==1);
                    auto ack=wire::decode_transition(wire::binary(wire::string(reply.values[0])));
                    CHECK(ack.tx==tx);
                    std::cout<<" tx="<<ack.tx<<" epoch="<<ack.next_epoch;
                }
                std::cout<<'\n'<<std::flush;
                code=status_name(reply.code)==args.at("--expect") ? 0 : 1;
            } CAPSTAN_CATCH_ALL {code=1;}
            finished=true;g_main_loop_quit(loop);
        });
        if(!finished) g_main_loop_run(loop);
        g_main_loop_unref(loop);return code;
    } CAPSTAN_CATCH_STD(e) {std::cerr<<e.what()<<'\n';return 1;}
}
