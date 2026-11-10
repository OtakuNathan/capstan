#define main capstan_host_application_main
#include "src/apps/host.cpp"
#undef main
#include "tests/check.hpp"
#include <future>
int main() {
    auto* context=g_main_context_new();
    {
        app::config c{{1,2,3},7,{}};for(unsigned i=0;i<3;++i)c.credentials[i][0]=static_cast<unsigned char>(i+10);
        host_runtime host(c,context,128*1024,0,32);host.trace_requests=false;
        grant g;g.key=c.key;g.epoch=1;g.grant_id=1;g.holder=1;g.mode=access_mode::exclusive;g.secret[0]=1;
        CHECK(host.authority.set_authority({c.key,7,1,0,1,false,{g}})==status::ok);
        migration::ticket t;t.control=7;t.sequence=1;t.source_host=1;t.source_instance=2;t.target_host=2;t.target_instance=3;t.generation=1;t.source=g;
        CHECK(host.authority.freeze_for_migration(t)==status::ok);
        auto send=[&](bytes data,io_completion done) {
            host.handle(0,wire::command(9999,{wire::value::string(app::token_bytes(c.credentials[2])),wire::value::string(wire::binary(data))}),std::move(done));
        };
        std::promise<void> entered,release;auto gate=release.get_future().share();auto ready=entered.get_future();
        CHECK(host.worker.try_dispatch(flux_foundry::task_wrapper_sbo([&entered,gate]() noexcept {entered.set_value();gate.wait();})));
        CHECK(ready.wait_for(std::chrono::seconds(5))==std::future_status::ready);
        unsigned exports=0,aborts=0;
        send(wire::command(migration::export_command,{wire::value::string(migration::encode_ticket(t))}),[&](io_result io) {
            CHECK(wire::decode_reply(io.payload).code==status::closed);++exports;
        });
        CHECK(host.authority.inspect().active==1 && exports==0);
        auto restored=g;restored.epoch=2;restored.grant_id=2;restored.secret[0]=2;
        send(wire::command(migration::abort_command,{wire::value::string(migration::encode_ticket(t)),wire::value::string(wire::encode_grant(restored))}),[&](io_result io) {
            auto response=wire::decode_reply(io.payload);
            CHECK(response.code==status::ok && response.values.size()==2);
            CHECK(wire::number(response.values[1])==static_cast<unsigned>(migration::authority_phase::aborted));
            CHECK(host.authority.inspect().active==0 && host.authority.inspect().epoch==2);
            // The ACK may reenter a fresh operation; the old native read is over.
            auto reentry=host.authority.admit({restored,90,method::version},1);CHECK(reentry.code==status::ok);
            host_authority::operation op;CHECK(host.authority.start_next(op));CHECK(host.authority.complete(op.pin)==status::ok);
            ++aborts;
        });
        CHECK(aborts==0 && exports==0 && host.authority.inspect().active==1);
        CHECK(host.authority.admit({restored,91,method::add_range},1).code==status::closed);
        release.set_value();auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(aborts!=1 || exports!=1) {
            while(g_main_context_iteration(context,FALSE)) {}
            CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(host.export_reserved==0 && host.authority.inspect().active==0);
        CHECK(host.exports.at(t.sequence)->code==status::closed && !host.exports.at(t.sequence)->payload);
    }
    for(bool mutable_access:{false,true}) {
        app::config c{{1,2,3},7,{}};
        host_runtime host(c,context,128*1024,0,32);host.trace_requests=false;
        grant g;g.key=c.key;g.epoch=1;g.grant_id=1;g.holder=1;g.mode=access_mode::exclusive;g.secret[0]=1;
        CHECK(host.authority.set_authority({c.key,7,1,0,1,false,{g}})==status::ok);
        migration::ticket t{7,1,1,2,2,3,1,g};
        CHECK(host.reserve_source(t,host.export_limit())==status::ok);
        auto read=host.authority.admit({g,100,method::summary},1);CHECK(read.code==status::ok);
        host_authority::operation op;CHECK(host.authority.start_next(op));
        host_authority::admission write{status::invalid};
        if(mutable_access) {write=host.authority.admit({g,101,method::add_range},1);CHECK(write.code==status::ok);}
        bool frozen=false;
        CHECK(host.migration_replies.freeze(t,[&](io_result io) {CHECK(wire::decode_reply(io.payload).code==status::ok);frozen=true;})==status::pending);
        host.prefetch_read_snapshot(t);
        auto record=host.exports.at(t.sequence);
        if(mutable_access) {
            CHECK(!record->prefetching && !record->capture_ready); // Queued write excludes speculation too.
            CHECK(host.authority.complete(read.pin)==status::ok);
            CHECK(host.authority.start_next(op) && op.pin==write.pin);
            CHECK(host.authority.complete(write.pin)==status::ok);
        } else {
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!record->capture_ready) {while(g_main_context_iteration(context,FALSE)) {} CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
            CHECK(!frozen && host.authority.inspect().active==1);
            CHECK(host.authority.fence_migration(t)==status::conflict);
            migration::authority_image history;CHECK(host.authority.export_authority(t,history)==status::pending);
            CHECK(host.authority.complete(read.pin)==status::ok);
        }
        host.collect_resource();CHECK(frozen);
        bool exported=false;
        host.export_snapshot(t,[&](io_result io) {CHECK(wire::decode_reply(io.payload).code==status::ok);exported=true;});
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!exported) {while(g_main_context_iteration(context,FALSE)) {} CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
        migration::dataset_snapshot data;migration::authority_image history;
        CHECK(migration::decode_package(*record->payload,data,history)==status::ok);
        CHECK(history.requests.count({1,100})==1);
        CHECK(history.requests.count({1,101})==static_cast<std::size_t>(mutable_access));
    }
    // A real native reader remains running until early capture has finished.
    // This exercises the independent lanes under TSan without timing sleeps.
    {
        app::config c{{1,2,3},7,{}};host_runtime host(c,context,128*1024,0,32);host.trace_requests=false;
        grant g;g.key=c.key;g.epoch=1;g.grant_id=1;g.holder=1;g.mode=access_mode::exclusive;g.secret[0]=1;
        CHECK(host.authority.set_authority({c.key,7,1,0,1,false,{g}})==status::ok);
        migration::ticket t{7,1,1,2,2,3,1,g};CHECK(host.reserve_source(t,host.export_limit())==status::ok);
        auto read=host.authority.admit({g,100,method::summary},1);CHECK(read.code==status::ok);
        host_authority::operation op;CHECK(host.authority.start_next(op));
        std::atomic<bool> stop{false};std::promise<void> entered,finished;
        auto entered_future=entered.get_future(),finished_future=finished.get_future();
        CHECK(host.worker.try_dispatch(flux_foundry::task_wrapper_sbo([&]() noexcept {
            entered.set_value();
            while(!stop.load()) {wire::value value;CHECK(host.owner->try_dispatch(method::summary,g,{},value));}
            finished.set_value();
        })));
        CHECK(entered_future.wait_for(std::chrono::seconds(5))==std::future_status::ready);
        bool frozen=false;CHECK(host.migration_replies.freeze(t,[&](io_result) {frozen=true;})==status::pending);
        host.prefetch_read_snapshot(t);
        auto record=host.exports.at(t.sequence);auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!record->capture_ready) {while(g_main_context_iteration(context,FALSE)) {} CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
        CHECK(!frozen && host.authority.inspect().active==1);
        stop.store(true);CHECK(finished_future.wait_for(std::chrono::seconds(5))==std::future_status::ready);
        CHECK(host.authority.complete(read.pin)==status::ok);host.collect_resource();CHECK(frozen);
    }
    // Abort after business Drain still waits for an already accepted early
    // capture; neither the observer nor a fresh writer can release that pin.
    {
        app::config c{{1,2,3},7,{}};host_runtime host(c,context,128*1024,0,32);host.trace_requests=false;
        grant g;g.key=c.key;g.epoch=1;g.grant_id=1;g.holder=1;g.mode=access_mode::exclusive;g.secret[0]=1;
        CHECK(host.authority.set_authority({c.key,7,1,0,1,false,{g}})==status::ok);
        migration::ticket t{7,1,1,2,2,3,1,g};CHECK(host.reserve_source(t,host.export_limit())==status::ok);
        auto read=host.authority.admit({g,100,method::summary},1);host_authority::operation op;CHECK(host.authority.start_next(op));
        std::promise<void> entered,release;auto gate=release.get_future().share();auto ready=entered.get_future();
        CHECK(host.capture_worker.get().try_dispatch(flux_foundry::task_wrapper_sbo([&entered,gate]() noexcept {entered.set_value();gate.wait();})));
        CHECK(ready.wait_for(std::chrono::seconds(5))==std::future_status::ready);
        unsigned freezes=0,aborts=0;CHECK(host.migration_replies.freeze(t,[&](io_result) {++freezes;})==status::pending);
        host.prefetch_read_snapshot(t);CHECK(host.authority.inspect().active==2);
        auto next=g;++next.epoch;++next.grant_id;next.secret[0]=2;
        CHECK(host.migration_replies.abort(t,next,[&](io_result) {++aborts;})==status::pending);
        CHECK(host.authority.complete(read.pin)==status::ok);host.collect_resource();
        CHECK(!freezes && !aborts && host.authority.inspect().active==1);
        CHECK(host.authority.admit({next,101,method::add_range},1).code==status::closed);
        release.set_value();auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!aborts) {while(g_main_context_iteration(context,FALSE)) {} CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
        CHECK(freezes==1 && aborts==1 && host.authority.inspect().active==0 && host.authority.inspect().epoch==2);
        CHECK(!host.exports.at(t.sequence)->capture_ready);
    }
    while(g_main_context_iteration(context,FALSE)) {}g_main_context_unref(context);
}
