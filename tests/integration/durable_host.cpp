#include "examples/owner/sqlite.hpp"
#include "src/apps/owner_host.hpp"
#include "src/apps/host_registry.hpp"
#include "tests/check.hpp"
#include <future>
#include <cstdio>
using backend_type=examples::sqlite_owner_backend;
using runtime_type=basic_host_runtime<backend_type>;
namespace {
app::config config() {app::config c{{1,2,3},4,{}};for(unsigned i=0;i<3;++i)c.credentials[i][0]=10+i;return c;}
grant initial() {grant g;g.key=config().key;g.epoch=1;g.holder=1;g.grant_id=11;g.mode=access_mode::exclusive;g.secret[0]=21;return g;}
grant successor() {auto g=initial();g.epoch=2;g.holder=2;g.grant_id=12;g.secret[0]=22;return g;}
std::string temporary() {char path[]="/tmp/capstan-host-XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);close(fd);return path;}
bytes auth(unsigned role,bytes call) {return wire::command(9999,{wire::value::string(app::token_bytes(config().credentials[role])),wire::value::string(wire::binary(call))});}
bytes write() {return wire::encode_call({initial(),1,method::add_range},{wire::u(0),wire::u(1),wire::u(7)});}
durable::fault_point crash_at;
#if CAPSTAN_HAS_EXCEPTIONS
void ambiguous(durable::fault_point point) {if(point==durable::fault_point::after_commit) throw std::runtime_error("commit result unavailable");}
#endif
void crash(durable::fault_point point) {if(point==crash_at) _exit(87);}
std::atomic<bool> committed{false};
void notify(durable::fault_point point) {if(point==durable::fault_point::after_commit) committed.store(true);}
void pump(GMainContext* c,const std::function<bool()>& done) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!done()) {while(g_main_context_iteration(c,FALSE)) {} CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(std::chrono::microseconds(100));}
}
wire::reply send(runtime_type& host,GMainContext* context,unsigned role,bytes call) {
    bool done=false;wire::reply result;
    host.handle(0,auth(role,std::move(call)),[&](io_result io) {CHECK(io.state==outcome::completed);CHECK(wire::try_decode_reply(io.payload,result));done=true;});
    pump(context,[&]{return done;});return result;
}
void bootstrap(const std::string& path) {
    auto* context=g_main_context_new();
    {auto backend=std::make_shared<backend_type>(path);runtime_type host(config(),context,1024,0,16,backend);host.trace_requests=false;
        auto t=transition{config().key,config().control,1,0,1,false,{initial()}};
        CHECK(send(host,context,2,wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition(t)))})).code==status::ok);
    }g_main_context_unref(context);
}
}
int main() {
    // All three mutation entry points implement one arithmetic/version contract.
    using native_dataset=portal::capstan_native::dataset;
    static_assert(noexcept(std::declval<native_dataset&>().publish_add(std::declval<const native_dataset::prepared_add&>())),"postcommit publication must not throw");
    static_assert(noexcept(native_dataset::replay_add(std::declval<migration::dataset_snapshot&>(),std::declval<const native_dataset::prepared_add&>())),"redo must not throw");
    for(auto version:{std::uint64_t(0),std::numeric_limits<std::uint64_t>::max()-1,std::numeric_limits<std::uint64_t>::max()})
    for(auto begin:{std::uint64_t(0),std::uint64_t(2),std::uint64_t(4),std::uint64_t(5)})
    for(auto end:{std::uint64_t(0),std::uint64_t(2),std::uint64_t(4),std::uint64_t(5)})
    for(auto delta:{std::uint64_t(0),std::uint64_t(7),std::numeric_limits<std::uint64_t>::max()}) {
        migration::dataset_snapshot original{version,{0,1,std::numeric_limits<std::uint64_t>::max(),41}};
        auto normal=native_dataset::restore(original),transactional=native_dataset::restore(original);
        auto replay=original;
        native_dataset::prepared_add prepared{123,456,789,999};
        const bool valid=begin<=end && end<=4 && version!=std::numeric_limits<std::uint64_t>::max();
        CHECK(normal->can_add_range(begin,end)==valid);
        CHECK(transactional->prepare_add(begin,end,delta,prepared)==valid);
        CHECK(native_dataset::replay_add(replay,{version,begin,end,delta})==valid);
        if(valid) {
            CHECK(normal->add_range(begin,end,delta)==version+1);
            transactional->publish_add(prepared);
            auto expected=original;
            for(std::size_t i=0;i<expected.words.size();++i) if(i>=begin && i<end) expected.words[i]+=delta;
            expected.version=version+1;
            CHECK(replay.version==expected.version && replay.words==expected.words);
            auto a=normal->capture(),b=transactional->capture();
            CHECK(a.version==b.version && a.words==b.words && a.version==replay.version && a.words==replay.words);
        } else {
            CHECK(prepared.before==123 && prepared.begin==456 && prepared.end==789 && prepared.delta==999);
            CHECK(replay.version==original.version && replay.words==original.words);
            auto untouched=transactional->capture();CHECK(untouched.version==original.version && untouched.words==original.words);
#if CAPSTAN_HAS_EXCEPTIONS
            bool rejected=false;
            try {(void)normal->add_range(begin,end,delta);}
            catch(const std::out_of_range&) {rejected=begin>end || end>4;}
            catch(const std::overflow_error&) {rejected=begin<=end && end<=4 && version==std::numeric_limits<std::uint64_t>::max();}
            CHECK(rejected);auto unchanged=normal->capture();CHECK(unchanged.version==original.version && unchanged.words==original.words);
#endif
        }
        auto mismatch=original;
        CHECK(!native_dataset::replay_add(mismatch,{version^1,begin,end,delta}));
        CHECK(mismatch.version==original.version && mismatch.words==original.words);
    }

    // Recovery imports a coherent authority checkpoint, never partial history.
    {
        host_authority source_core(config().key,config().control);
        CHECK(source_core.set_authority({config().key,config().control,1,0,1,false,{initial()}})==status::ok);
        host_authority::durable_image image;CHECK(source_core.checkpoint(image));
        auto wrong=image;wrong.transactions.clear();host_authority missing(config().key,config().control);
        CHECK(!missing.recover(wrong) && missing.inspect().epoch==0);
        wrong=image;wrong.grants[0].secret[1]=99;host_authority mismatched(config().key,config().control);
        CHECK(!mismatched.recover(wrong));
        auto encoded=durable::encode_host(image);host_authority::durable_image decoded;
        CHECK(durable::decode_host(encoded,decoded,16));host_authority restored(config().key,config().control);
        CHECK(restored.recover(decoded) && restored.inspect().grants==std::vector<grant>{initial()});
        encoded[8]=2;CHECK(!durable::decode_host(encoded,decoded,16));
        auto admitted=source_core.admit({initial(),1,method::version},1);CHECK(admitted.code==status::ok);
        CHECK(source_core.set_authority({config().key,config().control,2,1,2,false,{successor()}})==status::pending);
        CHECK(source_core.checkpoint(image) && image.gate==host_authority::phase::closing);
        wrong=image;wrong.transactions.erase(wrong.pending);host_authority no_intent(config().key,config().control);
        CHECK(!no_intent.recover(wrong));
        host_authority closing(config().key,config().control);CHECK(closing.recover(image));
        CHECK(closing.inspect().gate==host_authority::phase::closing && closing.inspect().active==0);
        closing.finish_recovery();CHECK(closing.inspect().epoch==2 && closing.inspect().grants==std::vector<grant>{successor()});
    }
    for(auto point:{durable::fault_point::before_commit,durable::fault_point::after_commit}) {
        auto path=temporary();bootstrap(path);
        auto pid=fork();CHECK(pid>=0);
        if(pid==0) {auto* c=g_main_context_new();auto b=std::make_shared<backend_type>(path);runtime_type host(config(),c,1024,0,16,b);
            crash_at=point;b->fault=crash;send(host,c,0,write());_exit(99);}
        int state;CHECK(waitpid(pid,&state,0)==pid && WIFEXITED(state) && WEXITSTATUS(state)==87);
        auto* c=g_main_context_new();
        {auto b=std::make_shared<backend_type>(path);runtime_type host(config(),c,2048,0,16,b);host.trace_requests=false;
            CHECK(host.owner->resource_bytes()==1024); // Never reseed a restored object.
            wire::value value;CHECK(host.owner->native->try_dispatch(method::version,initial(),{},value));
            CHECK(wire::number(value)==(point==durable::fault_point::before_commit ? 0:1));
            auto first=send(host,c,0,write());CHECK(first.state==outcome::completed && first.code==status::ok && wire::number(first.values.at(0))==1);
            auto again=send(host,c,0,write());CHECK(again.code==status::ok && wire::number(again.values.at(0))==1);
            auto changed=wire::encode_call({initial(),1,method::add_range},{wire::u(0),wire::u(1),wire::u(8)});
            CHECK(send(host,c,0,changed).code==status::conflict);
            CHECK(send(host,c,1,write()).code==status::bad_grant);
            CHECK(host.owner->native->try_dispatch(method::version,initial(),{},value) && wire::number(value)==1);
            CHECK(send(host,c,0,wire::command(migration::copy_command)).code==status::forbidden);
            basic_host_registry<runtime_type> registry(host);
            for(auto command:{migration::clone_command,migration::route_command,wire::transfer_exclusive}) {
                bool denied=false;
                registry.handle(0,auth(0,wire::command(command)),[&](io_result io) {CHECK(wire::decode_reply(io.payload).code==status::forbidden);denied=true;});
                CHECK(denied && b->healthy());
            }
            CHECK(!b->create(config().key,1024,7) && b->healthy());
        }g_main_context_unref(c);
        std::remove(path.c_str());std::remove((path+".host.lock").c_str());
    }
    // Backend committed, but posted owner-loop completion/pin retirement has
    // not run. Close persists; crash removes the native work and volatile pin.
    auto path=temporary();bootstrap(path);auto pid=fork();CHECK(pid>=0);
    if(pid==0) {
        auto* c=g_main_context_new();auto b=std::make_shared<backend_type>(path);runtime_type host(config(),c,1024,0,16,b);host.trace_requests=false;
        b->fault=notify;
        host.handle(0,auth(0,write()),[](io_result){_exit(91);});
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!committed.load()) {CHECK(std::chrono::steady_clock::now()<end);std::this_thread::yield();}
        b->fault=nullptr;
        transition t{config().key,config().control,2,1,2,false,{successor()}};
        host.handle(0,auth(2,wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition(t)))})),[](io_result){_exit(92);});
        CHECK(host.authority.inspect().gate==host_authority::phase::closing && host.authority.inspect().active==1);
        _exit(87);
    }
    int state;CHECK(waitpid(pid,&state,0)==pid && WIFEXITED(state) && WEXITSTATUS(state)==87);
    auto* c=g_main_context_new();
    {auto b=std::make_shared<backend_type>(path);runtime_type host(config(),c,1024,0,16,b);host.trace_requests=false;
        CHECK(host.authority.inspect().epoch==2 && host.authority.inspect().grants==std::vector<grant>{successor()});
        CHECK(send(host,c,0,write()).code==status::ok); // Durable replay even under the old epoch.
        CHECK(send(host,c,0,wire::encode_call({initial(),2,method::version})).code==status::stale);
        auto reply=send(host,c,1,wire::encode_call({successor(),2,method::version}));CHECK(reply.code==status::ok && wire::number(reply.values.at(0))==1);
        auto ack=send(host,c,2,wire::command(wire::query_ack,{wire::u(2)}));CHECK(ack.code==status::ok);
    }g_main_context_unref(c);std::remove(path.c_str());std::remove((path+".host.lock").c_str());
    // Immediate installation: neither an ACK nor an open gate can escape a
    // failed durable checkpoint. The before/after boundary decides recovery.
    for(auto point:{durable::fault_point::before_commit,durable::fault_point::after_commit}) {
        auto file=temporary();bootstrap(file);auto child=fork();CHECK(child>=0);
        if(child==0) {
            auto* context=g_main_context_new();auto b=std::make_shared<backend_type>(file);runtime_type host(config(),context,1024,0,16,b);
            crash_at=point;b->fault=crash;transition t{config().key,config().control,2,1,2,false,{successor()}};
            send(host,context,2,wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition(t)))}));_exit(99);
        }
        int result;CHECK(waitpid(child,&result,0)==child && WIFEXITED(result) && WEXITSTATUS(result)==87);
        auto* context=g_main_context_new();
        {auto b=std::make_shared<backend_type>(file);runtime_type host(config(),context,1024,0,16,b);
            CHECK(host.authority.inspect().epoch==(point==durable::fault_point::before_commit ? 1:2));
        }g_main_context_unref(context);std::remove(file.c_str());std::remove((file+".host.lock").c_str());
    }
#if CAPSTAN_HAS_EXCEPTIONS
    // An ambiguous COMMIT result closes the frontend, resolves waiters Unknown,
    // and retires only work whose native execution has actually finished.
    auto file=temporary();bootstrap(file);auto* context=g_main_context_new();
    {auto b=std::make_shared<backend_type>(file);runtime_type host(config(),context,1024,0,16,b);host.trace_requests=false;
        std::promise<void> entered,release;auto ready=entered.get_future();auto gate=release.get_future().share();
        CHECK(host.worker.try_dispatch(flux_foundry::task_wrapper_sbo([&entered,gate]() noexcept {entered.set_value();gate.wait();})));
        CHECK(ready.wait_for(std::chrono::seconds(5))==std::future_status::ready);
        unsigned callbacks=0;
        auto unknown=[&](io_result io) {CHECK(io.state==outcome::unknown || wire::decode_reply(io.payload).state==outcome::unknown);++callbacks;};
        host.handle(0,auth(0,write()),unknown);
        host.handle(0,auth(0,wire::encode_call({initial(),2,method::version})),unknown);
        transition t{config().key,config().control,2,1,2,false,{successor()}};
        host.handle(0,auth(2,wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition(t)))})),unknown);
        CHECK(callbacks==0 && host.authority.inspect().active==2);
        b->fault=ambiguous;release.set_value();pump(context,[&]{return callbacks==3;});
        CHECK(!b->healthy() && host.authority.inspect().active==0 && host.control_replies.size()==0);
        host.handle(0,auth(1,wire::encode_call({successor(),3,method::version})),unknown);CHECK(callbacks==4);
    }
    {auto b=std::make_shared<backend_type>(file);runtime_type host(config(),context,1024,0,16,b);host.trace_requests=false;
        CHECK(host.authority.inspect().epoch==2 && send(host,context,0,write()).code==status::ok);
        auto current=send(host,context,1,wire::encode_call({successor(),3,method::version}));CHECK(wire::number(current.values.at(0))==1);
    }
    g_main_context_unref(context);std::remove(file.c_str());std::remove((file+".host.lock").c_str());
#endif
    // Incremental storage preserves the exact Dataset image, including empty
    // ranges and modulo-2^64 arithmetic, across multiple reopen/retry cycles.
    {
        auto file=temporary();examples::single_process_owner_backend memory;
        auto expected=memory.create(config().key,1024,19);
        // Start from a legacy, nonzero-version snapshot and a retained receipt.
        wire::value v;CHECK(expected->try_dispatch(method::add_range,initial(),{wire::u(0),wire::u(4),wire::u(3)},v));
        bytes base;CHECK(expected->capture_snapshot(base)==status::ok);
        wire::call prior{{initial(),1,method::add_range},{wire::u(0),wire::u(4),wire::u(3)}};
        auto prior_response=wire::encode_reply(outcome::completed,status::ok,{wire::u(1)});
        durable::image_writer old_receipt(0x31524343);old_receipt.blob(wire::encode_call(prior.header,prior.args));old_receipt.blob(prior_response);
        durable::image_writer identity(0x31444943);identity.number(1);identity.number(2);identity.number(3);
        {
            durable::sqlite_store store(file);
            CHECK(store.commit({{"host/identity",identity.value},{"host/data",base},{"host/receipt/1/1",old_receipt.value}}).outcome==durable::commit_outcome::committed);
        }
        for(unsigned cycle=0;cycle<3;++cycle) {
            backend_type b(file);auto object=b.create(config().key,2048,999);CHECK(object);
            host_authority authority(config().key,config().control);CHECK(b.initialize(authority,config().control,128));
            CHECK(b.execute(*object,prior)==prior_response);
            auto conflict=prior;conflict.args[2]=wire::u(4);
            CHECK(wire::decode_reply(b.execute(*object,conflict)).code==status::conflict);
            bytes actual;CHECK(object->capture_snapshot(actual)==status::ok);
            bytes reference;CHECK(expected->capture_snapshot(reference)==status::ok);CHECK(actual==reference);
            for(unsigned i=0;i<4;++i) {
                const id rid=10+cycle*4+i,begin=i==0 ? 0:2,end=i==0 ? 0:17,delta=i==2 ? std::numeric_limits<id>::max():7;
                wire::call call{{initial(),rid,method::add_range},{wire::u(begin),wire::u(end),wire::u(delta)}};
                auto result=b.execute(*object,call);CHECK(wire::decode_reply(result).code==status::ok);
                CHECK(expected->try_dispatch(method::add_range,initial(),call.args,v));
                CHECK(b.execute(*object,call)==result); // No second effect.
            }
            wire::call invalid{{initial(),100+cycle,method::add_range},{wire::u(0),wire::u(129),wire::u(1)}};
            CHECK(wire::decode_reply(b.execute(*object,invalid)).code==status::invalid);
            CHECK(object->capture_snapshot(actual)==status::ok && expected->capture_snapshot(reference)==status::ok && actual==reference);
            std::vector<durable::record> deltas;CHECK(b.store->list("host/delta/",deltas));CHECK(deltas.size()==4*(cycle+1));
            bytes head;bool found;CHECK(b.store->get("host/data",head,found)&&found);
            migration::dataset_snapshot legacy;
            CHECK(migration::decode(head,legacy)!=status::ok); // Old binary refuses, cannot silently read stale base.
        }
        // Missing/torn tail is detected using the durable head, not silently
        // accepted as an older state. Normal commits cannot produce this image.
        {durable::sqlite_store store(file);CHECK(store.commit({{"host/data",backend_type::manifest(1,99)}}).outcome==durable::commit_outcome::committed);}
        {backend_type b(file);CHECK(!b.create(config().key,1024,19)&&!b.healthy());}
        // A correctly sized log still cannot contain a wrong predecessor, or
        // a delta pointing at a missing committed request receipt.
        const std::string first_delta="host/delta/00000000000000000002";
        bytes original;
        {durable::sqlite_store store(file);bool found;CHECK(store.get(first_delta,original,found)&&found);
            auto bad=original;bad[16]^=1;
            CHECK(store.commit({{"host/data",backend_type::manifest(1,13)},{first_delta,bad}}).outcome==durable::commit_outcome::committed);}
        {backend_type b(file);CHECK(!b.create(config().key,1024,19)&&!b.healthy());}
        {durable::sqlite_store store(file);
            durable::image_writer missing(0x314c4443);missing.number(1);missing.number(0);missing.number(0);missing.number(7);missing.number(1);missing.number(999);
            CHECK(store.commit({{first_delta,missing.value}}).outcome==durable::commit_outcome::committed);}
        {backend_type b(file);CHECK(!b.create(config().key,1024,19)&&!b.healthy());}
        std::remove(file.c_str());std::remove((file+".host.lock").c_str());
    }
    // Legacy format upgrade itself is atomic at the existing crash boundaries.
    for(auto point:{durable::fault_point::before_commit,durable::fault_point::after_commit}) {
        auto file=temporary();examples::single_process_owner_backend memory;auto native=memory.create(config().key,1024,23);
        bytes image;CHECK(native->capture_snapshot(image)==status::ok);
        durable::image_writer identity(0x31444943);identity.number(1);identity.number(2);identity.number(3);
        {durable::sqlite_store store(file);CHECK(store.commit({{"host/identity",identity.value},{"host/data",image}}).outcome==durable::commit_outcome::committed);}
        auto child=fork();CHECK(child>=0);
        if(child==0) {backend_type b(file);b.fault=crash;crash_at=point;(void)b.create(config().key,1024,23);_exit(99);}
        int result;CHECK(waitpid(child,&result,0)==child&&WIFEXITED(result)&&WEXITSTATUS(result)==87);
        {durable::sqlite_store store(file);bytes data;bool found;CHECK(store.get("host/data",data,found)&&found);
            migration::dataset_snapshot decoded;CHECK((migration::decode(data,decoded)==status::ok)==(point==durable::fault_point::before_commit));}
        {backend_type b(file);auto restored=b.create(config().key,1024,23);CHECK(restored);bytes out;CHECK(restored->capture_snapshot(out)==status::ok&&out==image);}
        std::remove(file.c_str());std::remove((file+".host.lock").c_str());
    }
    std::cout<<"Durable RPC replay, payload binding, Close recovery and real Drain passed\n";
}
