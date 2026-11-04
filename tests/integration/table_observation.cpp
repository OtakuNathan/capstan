#define CAPSTAN_TABLE_NO_MAIN
#include "src/apps/table.cpp"
#include "tests/check.hpp"
int main() {
    auto* context=g_main_context_new();
    {
        app::config config{};config.key={1,2,3};config.control=4;
        // Construct a valid connection, then close it deliberately in both EH
        // and no-EH builds. Inject outcomes into the real Table completion path.
        gio_server server(context,0,[](id,bytes,io_completion){std::abort();});CHECK(server.valid());
        table_runtime table(config,context,server.port());table.host_socket.close();
        auto op=std::make_shared<table_runtime::transition_operation>();op->command={1,1};op->started=true;
        CHECK(table.directory.begin({},false,op->target)==status::ok);
        table.history.emplace(op->command,table_runtime::command_record{});
        auto expect=[&](outcome state,status code) {
            return [state,code](io_result io) {wire::reply reply;CHECK(wire::try_decode_reply(io.payload,reply));CHECK(reply.state==state && reply.code==code);};
        };
        op->done=expect(outcome::unknown,status::pending);
        table.on_ack(op,{outcome::unknown,{}});
        CHECK(table.changing && op->ever_uncertain && table.uncertain_operation==op);
        op->done=expect(outcome::unknown,status::pending);
        table.on_ack(op,{outcome::not_submitted,{}});
        transition retry;CHECK(table.directory.retry(retry)==status::ok && retry==op->target);
        op->done=expect(outcome::unknown,status::pending);
        table.on_ack(op,{outcome::completed,wire::encode_reply(outcome::rejected,status::busy)});
        CHECK(table.changing && table.directory.retry(retry)==status::ok);
        // A valid-looking payload cannot turn an uncertain/rejected envelope
        // into proof of installation. Keep the exact original decision pending.
        for(auto state:{outcome::unknown,outcome::not_submitted,outcome::rejected}) {
            op->done=expect(outcome::unknown,status::pending);
            table.on_ack(op,{outcome::completed,wire::encode_reply(state,status::ok,
                {wire::value::string(wire::binary(wire::encode_transition(op->target)))})});
            CHECK(table.changing && table.directory.epoch()==0 && table.directory.retry(retry)==status::ok);
        }
        // Simulate an already-observed ACK followed by failure before completion
        // of the subsequent current-state query. Replayed ACK must remain useful.
        CHECK(table.directory.receive({op->target})==status::ok);
        op->done=expect(outcome::not_submitted,status::busy);
        table.on_ack(op,{outcome::completed,wire::encode_reply(outcome::completed,status::ok,
            {wire::value::string(wire::binary(wire::encode_transition(op->target)))})});
        CHECK(!table.changing && !table.uncertain_operation && table.directory.epoch()==1);
        // A first-attempt reliable rejection does clear its exact pending record.
        auto fresh=std::make_shared<table_runtime::transition_operation>();fresh->command={1,2};fresh->started=true;
        CHECK(table.directory.begin_at(1,{},false,fresh->target)==status::ok);
        table.history.emplace(fresh->command,table_runtime::command_record{});table.changing=true;
        fresh->done=expect(outcome::rejected,status::stale);
        table.on_ack(fresh,{outcome::completed,wire::encode_reply(outcome::rejected,status::stale)});
        CHECK(!table.changing && table.directory.retry(retry)==status::invalid);
    }
    {
        app::config config{};config.key={1,2,3};config.control=4;
        outcome response_state=outcome::unknown;
        authority_view view{config.key,1,0,{}};
        gio_server server(context,0,[&](id,bytes,io_completion reply) {
            reply({outcome::completed,wire::encode_reply(response_state,status::ok,
                {wire::value::string(wire::encode_authority_view(view))})});
        });CHECK(server.valid());
        table_runtime table(config,context,server.port());
        for(auto state:{outcome::unknown,outcome::not_submitted,outcome::rejected,outcome::completed}) {
            response_state=state;bool finished=false;
            table.snapshot([&](outcome observed,status code,authority_view current) {
                if(state==outcome::completed) CHECK(observed==state && code==status::ok && current.epoch==1);
                else CHECK(observed==outcome::unknown && code==status::invalid && table.directory.epoch()==0);
                finished=true;
            });
            while(!finished) g_main_context_iteration(context,TRUE);
        }
    }
    g_main_context_unref(context);
}
