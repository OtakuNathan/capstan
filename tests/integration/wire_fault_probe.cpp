#include "src/apps/common.hpp"
#include "tests/check.hpp"
using namespace capstan;
int main(int argc,char** argv) {
    auto args=app::options(argc,argv);auto config=app::load_config(app::required(args,"--config"));
    auto* loop=g_main_loop_new(nullptr,FALSE);auto* context=g_main_loop_get_context(loop);
    gio_client host(context,"127.0.0.1",app::port_option(args,"--host-port",0));
    gio_client table(context,"127.0.0.1",app::port_option(args,"--table-port",0));
    CHECK(host.valid() && table.valid());
    unsigned completed=0;
    auto check=[&](io_endpoint& endpoint,bytes packet,status expected) {
        bool finished=false;
        start_io(endpoint,std::move(packet),[&](io_result io) noexcept {
            wire::reply reply;CHECK(io.state==outcome::completed && wire::try_decode_reply(io.payload,reply));
            CHECK(reply.code==expected);finished=true;++completed;g_main_loop_quit(loop);
        });
        if(!finished) g_main_loop_run(loop);
    };
    auto authenticated=[&](bytes payload,unsigned role) {
        return wire::command(9999,{wire::value::string(app::token_bytes(config.credentials[role])),wire::value::string(wire::binary(payload))});
    };
    check(host,{0},status::invalid);check(table,{0},status::invalid);
    check(host,authenticated({0},2),status::invalid);
    check(table,authenticated({0},0),status::invalid);
    check(host,authenticated(wire::command(wire::set_authority),2),status::invalid);
    check(host,authenticated(wire::command(wire::set_authority,{wire::value::boolean(true)}),2),status::invalid);
    check(host,authenticated(wire::command(wire::set_authority,{wire::value::string("broken transition")}),2),status::invalid);
    check(host,authenticated(wire::command(wire::query_ack,{wire::value::boolean(true)}),2),status::invalid);
    check(table,authenticated(wire::command(wire::configure_grants,{wire::u(1)}),0),status::invalid);
    check(table,authenticated(wire::command(wire::configure_grants,{wire::u(1),wire::u(0)}),0),status::invalid);
    check(table,authenticated(wire::command(wire::configure_grants,{wire::u(1),wire::value::boolean(false),wire::u(1)}),0),status::invalid);
    check(table,authenticated(wire::command(2002,{wire::u(1),wire::value::string("broken grant")}),0),status::invalid);
    check(host,authenticated(wire::command(wire::stats),0),status::ok);
    check(table,authenticated(wire::command(wire::stats),0),status::ok);
    for(auto method:{wire::transfer_exclusive,wire::query_transfer,wire::claim_transfer}) {
        check(host,authenticated(wire::command(method),0),status::invalid);
        check(host,authenticated(wire::command(method,{wire::value::string("truncated")}),0),status::invalid);
    }
    check(host,authenticated(wire::command(wire::stats),0),status::ok);
    CHECK(completed==21);g_main_loop_unref(loop);
    std::cout<<"WIRE_FAULTS completed="<<completed<<" alive=1\n";
}
