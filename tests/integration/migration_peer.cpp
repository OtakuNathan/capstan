#define main capstan_host_application_main
#include "src/apps/host.cpp"
#undef main
#include "tests/check.hpp"
#include <capstan/migration/coordinator.hpp>
struct controlled_peer : io_endpoint {
    gio_client socket;app::authenticated_endpoint auth;id lose=0;
    controlled_peer(GMainContext* ctx,unsigned short port,token credential):socket(ctx,"127.0.0.1",port),auth(socket,credential) {}
    bool submit(bytes bytes,io_completion done) noexcept override {
        wire::codec::request request;bool decoded=wire::try_decode(bytes,request);
        if(decoded && request.method==migration::route_command && request.args.size()==4) decoded=wire::try_decode(wire::binary(wire::string(request.args[3])),request);
        bool drop=decoded && request.method==lose;
        if(drop) lose=0;
        return auth.submit(std::move(bytes),[drop,done=std::move(done)](io_result r) mutable {done(drop ? io_result{outcome::unknown,{}} : std::move(r));});
    }
};
int main() {
    auto* context=g_main_context_new();
    {
        app::config c{{1,2,3},7,{}};for(unsigned i=0;i<3;++i) c.credentials[i][0]=static_cast<unsigned char>(i+10);
        host_runtime source(c,context,128*1024,0,32),target(c,context,0,0,32);
        source.host_id=1;source.host_instance=11;target.host_id=2;target.host_instance=12;
        source.trace_requests=false;target.trace_requests=false;
        gio_server a(context,0,[&](id id,bytes b,io_completion done) {source.handle(id,std::move(b),std::move(done));});
        gio_server b(context,0,[&](id id,bytes bytes,io_completion done) {target.handle(id,std::move(bytes),std::move(done));});
        CHECK(a.valid() && b.valid());
        gio_client source_socket(context,"127.0.0.1",a.port()),target_socket(context,"127.0.0.1",b.port());
        app::authenticated_endpoint from(source_socket,c.credentials[2]),to(target_socket,c.credentials[2]);
        auto pump=[&](auto predicate) {
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!predicate()) {while(g_main_context_iteration(context,FALSE)) {} CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        };
        auto call=[&](io_endpoint& ep,bytes bytes) {
            bool ready=false;wire::reply result;
            CHECK(ep.submit(std::move(bytes),[&](io_result r) {CHECK(r.state==outcome::completed);CHECK(wire::try_decode_reply(r.payload,result));ready=true;}));
            pump([&]{return ready;});return result;
        };
        grant g;g.key=c.key;g.epoch=1;g.grant_id=1;g.holder=1;g.mode=access_mode::exclusive;g.secret[0]=1;
        CHECK(source.authority.set_authority({c.key,7,1,0,1,false,{g}})==status::ok);
        migration::ticket t{7,1,1,11,2,12,1,g};auto next=g;next.epoch=2;next.grant_id=2;next.secret[0]=2;
        auto command=[&](id command) {return wire::command(command,{wire::value::string(migration::encode_ticket(t))});};
        auto reserve=wire::command(migration::reserve_command,{wire::value::string(migration::encode_ticket(t)),wire::value::string(wire::encode_grant(next)),wire::u(256*1024)});
        CHECK(call(to,wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition({c.key,7,99,0,1,false,{g}})))})).code==status::closed);
        CHECK(call(to,reserve).code==status::ok);
        CHECK(target.authority.set_authority({c.key,7,2,0,1,false,{g}})==status::busy);
        CHECK(call(to,command(migration::activate_command)).code==status::closed);
        CHECK(call(from,command(migration::freeze_command)).code==status::ok);
        auto pull=wire::command(migration::pull_command,{wire::value::string(migration::encode_ticket(t)),wire::value::string("127.0.0.1"),wire::u(a.port())});
        CHECK(call(to,pull).code==status::ok);
        CHECK(call(to,pull).code==status::ok); // ACK retry does not restore again.
        CHECK(target.imports.at(1)->phase==host_runtime::import_phase::prepared);
        CHECK(!target.owner && target.authority.admit({next,1,method::version},1).code==status::closed);
        CHECK(call(from,command(migration::fence_command)).code==status::ok);
        CHECK(call(to,command(migration::activate_command)).code==status::ok);
        CHECK(call(to,command(migration::activate_command)).code==status::ok);
        CHECK(target.owner && target.owner->resource_bytes()==128*1024 && !target.import_reserved);
        CHECK(target.authority.placement_generation()==2 && source.authority.inspect().gate==host_authority::phase::closing);
        gio_client client(context,"127.0.0.1",b.port());app::authenticated_endpoint business(client,c.credentials[0]);
        auto result=call(business,wire::encode_call({next,20,method::summary}));CHECK(result.code==status::ok && result.values.size()==1);
        dataset_backend::context conversion;auto value=portal::from_optional<summary_t>(conversion,result.values[0]);CHECK(value);
        auto expected=portal::capstan_native::dataset(128*1024,7).summary();
        CHECK(value->sum==expected.sum && value->checksum==expected.checksum && value->version==expected.version && value->count==expected.count);
        CHECK(call(from,command(migration::retire_command)).code==status::ok && !source.owner);
        CHECK(call(from,command(migration::export_release_command)).code==status::ok && !source.export_reserved);
        // A return migration reserves the original fenced placement, then aborts.
        t={7,2,2,12,1,11,2,next};auto rollback=next;rollback.epoch=3;rollback.grant_id=3;rollback.secret[0]=3;
        reserve=wire::command(migration::reserve_command,{wire::value::string(migration::encode_ticket(t)),wire::value::string(wire::encode_grant(rollback)),wire::u(256*1024)});
        CHECK(call(from,reserve).code==status::ok);
        CHECK(call(from,command(migration::import_abort_command)).code==status::ok);
        CHECK(call(from,reserve).code==status::closed);
        CHECK(!source.import_reserved && !source.owner && source.authority.inspect().gate==host_authority::phase::closing);
        // An Abort preceding Begin is still terminal.
        ++t.sequence;CHECK(call(from,command(migration::import_abort_command)).code==status::ok);
        reserve=wire::command(migration::reserve_command,{wire::value::string(migration::encode_ticket(t)),wire::value::string(wire::encode_grant(rollback)),wire::u(256*1024)});
        CHECK(call(from,reserve).code==status::conflict);
        // The Table record survives a lost activation ACK and cannot roll back
        // its committed move. A retry resumes the exact checkpoint.
        ++t.sequence;
        auto source_peer=std::make_shared<controlled_peer>(context,b.port(),c.credentials[2]);
        auto destination_peer=std::make_shared<controlled_peer>(context,a.port(),c.credentials[2]);
        destination_peer->lose=migration::activate_command;
        auto move=std::make_shared<migration::move_operation>(t,rollback,source_peer,destination_peer,"127.0.0.1",b.port(),target.export_limit());
        CHECK(target.authority.inspect().gate==host_authority::phase::open); // inert Cut
        bool observed=false;wire::reply receipt;
        auto observe=[&](io_result io) {CHECK(wire::try_decode_reply(io.payload,receipt));observed=true;};
        CHECK(move->resume(observe));pump([&]{return observed;});
        CHECK(receipt.state==outcome::unknown && move->committed() && !move->ready());
        CHECK(source.owner && source.authority.placement_generation()==3);
        CHECK(!move->abort(observe)); // Commit is immutable, despite Unknown.
        observed=false;CHECK(move->resume(observe));pump([&]{return observed;});
        CHECK(receipt.code==status::ok && move->ready() && move->checkpoint()==migration::move_operation::step::ready);
        CHECK(!target.owner && !target.export_reserved && !source.import_reserved);
        CHECK(source.authority.admit({rollback,20,method::summary},1).code==status::duplicate);
        // Late retirement from the first placement must not delete the return.
        auto old=t;old={7,1,1,11,2,12,1,g};
        CHECK(call(from,wire::command(migration::retire_command,{wire::value::string(migration::encode_ticket(old))})).code==status::conflict);
        CHECK(source.owner);
        CHECK(call(to,wire::command(migration::activate_command,{wire::value::string(migration::encode_ticket(old))})).code==status::superseded);
        // Lost reservation ACK is precommit uncertainty. Abort first leaves a
        // target tombstone, then restores the source with a fresh grant.
        auto after_abort=rollback;++after_abort.epoch;++after_abort.grant_id;after_abort.secret[0]=4;
        t={7,5,1,11,2,12,3,rollback};source_peer->lose=migration::reserve_command;
        auto cancel=std::make_shared<migration::move_operation>(t,after_abort,destination_peer,source_peer,"127.0.0.1",a.port(),source.export_limit());
        observed=false;CHECK(cancel->resume(observe));pump([&]{return observed;});
        CHECK(receipt.state==outcome::unknown && !cancel->committed());
        CHECK(source.authority.inspect().gate==host_authority::phase::open);
        observed=false;CHECK(cancel->abort(observe));pump([&]{return observed;});
        CHECK(receipt.code==status::ok && cancel->checkpoint()==migration::move_operation::step::aborted);
        CHECK(source.authority.inspect().epoch==4 && !source.export_reserved && !target.import_reserved);
        CHECK(source.authority.admit({rollback,30,method::version},1).code==status::stale);
        CHECK(call(to,wire::command(migration::reserve_command,{wire::value::string(migration::encode_ticket(t)),wire::value::string(wire::encode_grant(after_abort)),wire::u(source.export_limit())})).code==status::closed);

    }
    while(g_main_context_iteration(context,FALSE)) {}g_main_context_unref(context);
    std::cout<<"Direct Host-to-Host snapshot pull, Prepared gate, activation and tombstones passed\n";
}
