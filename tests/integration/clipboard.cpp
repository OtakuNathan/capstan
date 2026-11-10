#define main capstan_host_application_main
#include "src/apps/host.cpp"
#undef main
#include "tests/check.hpp"
#include <capstan/migration/coordinator.hpp>
#include "examples/owner/mirrored_memory.hpp"
struct registry_session : io_endpoint {
    gio_client socket;app::authenticated_endpoint auth;
    registry_session(GMainContext* ctx,unsigned short port,token credential):socket(ctx,"127.0.0.1",port),auth(socket,credential) {}
    bool submit(bytes packet,io_completion done) noexcept override {return auth.submit(std::move(packet),std::move(done));}
};
template<class Backend,class Verify>
void exercise(std::shared_ptr<Backend> source_backend,std::shared_ptr<Backend> target_backend,Verify verify) {
    using host_runtime=basic_host_runtime<Backend>;
    using host_registry=basic_host_registry<host_runtime>;
    auto* context=g_main_context_new();
    {
        app::config c{{1,2,3},7,{}};for(unsigned i=0;i<3;++i) c.credentials[i][0]=static_cast<unsigned char>(i+10);
        host_runtime source(c,context,128*1024,0,32,source_backend),target(c,context,0,0,32,target_backend);
        source.host_id=1;source.host_instance=11;target.host_id=2;target.host_instance=12;
        source.trace_requests=false;target.trace_requests=false;
        host_registry sources(source),targets(target);
        bool delay_chunk=false;io_completion held_chunk;io_result held_response;
        gio_server a(context,0,[&](id id,bytes bytes,io_completion done) {
            app::authenticated auth;wire::codec::request request;
            CHECK(app::try_authenticate(c,bytes,auth) && wire::try_decode(auth.payload,request));
            if(request.method==migration::route_command) CHECK(wire::try_decode(wire::binary(wire::string(request.args[3])),request));
            if(request.method==migration::copy_chunk_command) CHECK(auth.holder!=3); // Never disclose the controller credential to a caller-selected peer.
            const bool delay=delay_chunk && request.method==migration::copy_chunk_command;
            if(delay) delay_chunk=false;
            sources.handle(id,std::move(bytes),[&,delay,done=std::move(done)](io_result io) mutable {
                if(delay) {held_response=std::move(io);held_chunk=std::move(done);}else done(std::move(io));
            });
        });
        gio_server b(context,0,[&](id id,bytes bytes,io_completion done) {targets.handle(id,std::move(bytes),std::move(done));});
        CHECK(a.valid() && b.valid());
        gio_client source_socket(context,"127.0.0.1",a.port()),target_socket(context,"127.0.0.1",b.port()),other_socket(context,"127.0.0.1",a.port());
        app::authenticated_endpoint from(source_socket,c.credentials[0]),to(target_socket,c.credentials[0]),other(other_socket,c.credentials[1]);
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
        auto expected=call(from,wire::encode_call({g,1,method::summary}));CHECK(expected.code==status::ok);
        auto copy_packet=wire::command(migration::copy_command,{wire::value::string(wire::encode_grant(g)),wire::u(2)});
        auto copied=call(from,copy_packet);CHECK(copied.code==status::ok && copied.values.size()==1);
        migration::snapshot_ref snapshot;CHECK(migration::decode_snapshot_ref(wire::string(copied.values[0]),snapshot));
        CHECK(source.authority.inspect().gate==host_authority::phase::open && !source.authority.inspect().active);
        CHECK(call(from,copy_packet).code==status::ok); // Exact Copy retry does not recapture.
        auto observed_copy=call(from,wire::command(migration::copy_query_command,{wire::u(2)}));
        CHECK(observed_copy.code==status::ok && wire::string(observed_copy.values[0])==wire::string(copied.values[0]));
        CHECK(call(other,wire::command(migration::copy_query_command,{wire::u(2)})).code==status::not_seen);
        CHECK(call(from,wire::encode_call({g,3,method::add_range},{wire::u(0),wire::u(1),wire::u(19)})).code==status::ok);
        auto changed=call(from,wire::encode_call({g,4,method::summary}));CHECK(changed.code==status::ok);
        CHECK(wire::encode_reply(expected.state,expected.code,expected.values)!=wire::encode_reply(changed.state,changed.code,changed.values));
        auto chunk=wire::command(migration::copy_chunk_command,{wire::value::string(migration::encode_snapshot_ref(snapshot)),wire::u(0)});
        CHECK(call(other,chunk).code==status::forbidden);
        auto paste=[&](id sequence,object_key key) {return wire::command(migration::clone_command,{wire::u(sequence),wire::value::string(migration::encode_snapshot_ref(snapshot)),
            wire::u(key.cluster),wire::u(key.incarnation),wire::u(key.object),wire::value::string("127.0.0.1"),wire::u(a.port())});};
        object_key first{1,2,4},second{1,2,5};
        const auto budget=target.export_budget;
        target.export_budget=snapshot.size+target.backend->restore_reservation(snapshot.size)-1;
        CHECK(call(to,paste(99,{1,2,99})).code==status::exhausted && targets.objects.empty());
        target.export_budget=budget;
        auto pasted=call(to,paste(1,first));CHECK(pasted.code==status::ok && pasted.values.size()==2);
        grant clone;CHECK(wire::try_decode_grant(wire::string(pasted.values[1]),clone) && clone.key==first && clone.epoch==1);
        auto retry=call(to,paste(1,first));CHECK(retry.code==status::ok && wire::string(retry.values[1])==wire::string(pasted.values[1]));
        CHECK(call(to,paste(1,second)).code==status::conflict);
        auto second_paste=call(to,paste(2,second));CHECK(second_paste.code==status::ok);
        grant clone2;CHECK(wire::try_decode_grant(wire::string(second_paste.values[1]),clone2) && clone2.key==second && clone2.secret!=clone.secret);
        for(const auto& copy:{clone,clone2}) {
            auto actual=call(to,migration::route(copy.key,wire::encode_call({copy,1,method::summary}))); // No copied source dedup.
            CHECK(actual.code==status::ok && wire::encode_reply(actual.state,actual.code,actual.values)==wire::encode_reply(expected.state,expected.code,expected.values));
        }
        CHECK(call(to,migration::route(first,wire::encode_call({clone,2,method::add_range},{wire::u(0),wire::u(1),wire::u(7)}))).code==status::ok);
        auto unchanged=call(to,migration::route(second,wire::encode_call({clone2,2,method::summary})));
        CHECK(unchanged.code==status::ok && wire::encode_reply(unchanged.state,unchanged.code,unchanged.values)==wire::encode_reply(expected.state,expected.code,expected.values));
        CHECK(targets.clone_reserved==0 && targets.objects.size()==2);
        for(const auto& item:targets.objects) CHECK(reinterpret_cast<std::uintptr_t>(item.second.runtime.get())%alignof(host_runtime)==0);
        // Migrate one dynamically routed clone while the sibling stays usable.
        auto successor=clone;++successor.epoch;successor.grant_id=clone.grant_id==1 ? 2 : 1;successor.secret[0]^=128;
        migration::ticket move_ticket{7,20,2,12,1,11,1,clone};
        auto move_source=std::make_shared<registry_session>(context,b.port(),c.credentials[2]);
        auto move_target=std::make_shared<registry_session>(context,a.port(),c.credentials[2]);
        auto move=std::make_shared<migration::move_operation>(move_ticket,successor,move_source,move_target,"127.0.0.1",b.port(),targets.objects.at(host_registry::key(first)).runtime->export_limit());
        bool moved=false;CHECK(move->resume([&](io_result io) {CHECK(wire::decode_reply(io.payload).code==status::ok);moved=true;}));pump([&]{return moved;});
        CHECK(move->ready() && sources.objects.at(host_registry::key(first)).runtime->owner);
        CHECK(!targets.objects.at(host_registry::key(first)).runtime->owner);
        CHECK(call(from,migration::route(first,wire::encode_call({successor,1,method::summary}))).code==status::duplicate);
        CHECK(call(to,migration::route(second,wire::encode_call({clone2,3,method::version}))).code==status::ok);
        CHECK(call(to,paste(1,first)).code==status::ok); // Historical creation receipt cannot republish it.
        CHECK(!targets.objects.at(host_registry::key(first)).runtime->owner);

        CHECK(call(to,wire::command(migration::clone_abort_command,{wire::u(3)})).code==status::ok);
        CHECK(call(to,paste(3,{1,2,6})).code==status::closed);
        CHECK(call(to,wire::command(migration::clone_abort_command,{wire::u(1)})).code==status::conflict);
        // Abort a partial clone while its first source chunk response is held.
        // The late response cannot resurrect the reserved destination slot.
        delay_chunk=true;bool late_completed=false;
        CHECK(to.submit(paste(4,{1,2,7}),[&](io_result io) {CHECK(wire::decode_reply(io.payload).code==status::closed);late_completed=true;}));
        pump([&]{return bool(held_chunk);});
        CHECK(targets.clone_reserved==snapshot.size+target.backend->restore_reservation(snapshot.size));
        gio_client abort_socket(context,"127.0.0.1",b.port());app::authenticated_endpoint abort_client(abort_socket,c.credentials[0]);
        CHECK(call(abort_client,wire::command(migration::clone_abort_command,{wire::u(4)})).code==status::ok);
        CHECK(!targets.clone_reserved && !targets.objects.at(host_registry::key({1,2,7})).published);
        auto late=std::exchange(held_chunk,nullptr);late(std::move(held_response));pump([&]{return late_completed;});
        CHECK(!targets.objects.at(host_registry::key({1,2,7})).runtime->owner);
        auto release=wire::command(migration::copy_release_command,{wire::value::string(migration::encode_snapshot_ref(snapshot))});
        CHECK(call(from,release).code==status::ok && !source.copy_reserved);
        CHECK(call(from,release).code==status::ok && call(from,copy_packet).code==status::closed);
        CHECK(call(from,chunk).code==status::closed);
        verify(source,target,sources,targets);
        // Aggregate quota covers all objects, snapshots and receive reservations.
        target.export_budget=targets.other_bytes(&target);
        CHECK(call(to,paste(5,{1,2,8})).code==status::exhausted);
    }
    while(g_main_context_iteration(context,FALSE)) {}g_main_context_unref(context);
    std::cout<<"Copy-time snapshots, reusable Paste, isolated clone identities, caller binding and explicit release passed\n";
}

int main() {
    using namespace capstan::examples;
    exercise(std::make_shared<single_process_owner_backend>(),std::make_shared<single_process_owner_backend>(),
             [](const auto&,const auto&,const auto&,const auto&){});
    auto audit=std::make_shared<mirrored_owner_audit>();
    exercise(std::make_shared<mirrored_memory_owner_backend>(audit),std::make_shared<mirrored_memory_owner_backend>(audit),
        [](const auto& source,const auto& target,const auto& sources,const auto& targets) {
            CHECK(source.owner->replicas_equal());
            if(target.owner) CHECK(target.owner->replicas_equal());
            for(const auto* registry:{&sources,&targets}) for(const auto& slot:registry->objects)
                if(slot.second.runtime->owner) {
                    CHECK(slot.second.runtime->owner->replicas_equal());
                    CHECK(slot.second.runtime->backend==registry->root.backend);
                }
        });
    CHECK(audit->created==1 && audit->restored>=3 && audit->writes>=2 && audit->live==0);
    std::cout<<"One logical owner, two private copies, opaque snapshots and inherited backend contexts passed\n";
}
