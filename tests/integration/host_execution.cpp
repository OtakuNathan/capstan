#define main capstan_host_application_main
#include "src/apps/host.cpp"
#undef main
#include "tests/check.hpp"
#include <future>

// A scalar read queued behind a blocked write must neither overtake it nor
// disappear from Drain when its execution placement changes.
int main() {
    for(bool inline_read:{false,true}) {
        auto* context=g_main_context_new();
        {
            app::config config{{1,2,3},4,{}};
            for(unsigned i=0;i<3;++i) config.credentials[i][0]=static_cast<unsigned char>(10+i);
            host_runtime host(config,context,1024,0,16);
            CHECK(host.worker.valid());
            host.trace_requests=false;host.inline_version=inline_read;
            grant source;source.key=config.key;source.epoch=1;source.holder=1;
            source.grant_id=1;source.mode=access_mode::exclusive;source.secret[0]=21;
            CHECK(host.authority.set_authority({config.key,config.control,1,0,1,false,{source}})==status::ok);
            std::promise<void> entered,release;
            auto ready=entered.get_future();auto gate=release.get_future().share();
            CHECK(host.worker.try_dispatch(flux_foundry::task_wrapper_sbo([&entered,gate]() noexcept {
                entered.set_value();gate.wait();
            })));
            CHECK(ready.wait_for(std::chrono::seconds(5))==std::future_status::ready);
            unsigned callbacks=0,acks=0;
            auto send=[&](unsigned role,bytes request,io_completion completion) {
                host.handle(0,wire::command(9999,{wire::value::string(app::token_bytes(config.credentials[role])),
                                                wire::value::string(wire::binary(request))}),std::move(completion));
            };
            auto check_version=[&](io_result io) {
                CHECK(io.state==outcome::completed);
                auto reply=wire::decode_reply(io.payload);
                std::uint64_t version=0;
                CHECK(typed_call<std::uint64_t>::try_decode(reply,version) && version==1);
                ++callbacks;
            };
            send(0,wire::encode_call({source,1,method::add_range},{wire::u(0),wire::u(1),wire::u(1)}),check_version);
            send(0,wire::encode_call({source,2,method::version},{}),check_version);
            CHECK(host.authority.inspect().active==2 && host.authority.inspect().executing!=0);
            auto target=source;target.epoch=2;target.holder=2;target.grant_id=2;target.secret[0]=22;
            transition next{config.key,config.control,2,1,2,false,{target}};
            send(2,wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition(next)))}),[&](io_result io) {
                CHECK(io.state==outcome::completed && wire::decode_reply(io.payload).code==status::ok);
                CHECK(host.authority.inspect().active==0 && host.authority.inspect().epoch==2);
                ++acks;
            });
            while(g_main_context_iteration(context,FALSE)) {}
            CHECK(callbacks==0 && acks==0 && host.authority.inspect().epoch==1);
            release.set_value();
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(callbacks!=2 || acks!=1) {
                while(g_main_context_iteration(context,FALSE)) {}
                CHECK(std::chrono::steady_clock::now()<deadline);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            CHECK(host.admitted==2 && host.completed==2 && host.authority.inspect().active==0);
        }
        while(g_main_context_iteration(context,FALSE)) {}
        g_main_context_unref(context);
    }
    std::cout<<"Worker/inline scalar reads preserve queued-write order and Drain ACK barrier\n";
}
