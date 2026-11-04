#include <capstan/error_policy.hpp>
#include <capstan/access/capability.hpp>
#include <capstan/async/detachable.hpp>
#include <capstan/authority/host.hpp>
#include <capstan/authority/directory.hpp>
#include "tests/check.hpp"
#include <map>
#include <memory>
using namespace capstan;
struct loopback_host {
    host_authority authority{{1,2,3},9};
    std::unique_ptr<dataset_binding> resource{new dataset_binding({1,2,3},1024,7)};
    struct queued { wire::call call; io_completion done; };
    std::map<id,queued> queue;
    std::size_t wire_bytes=0,submissions=0;
    bool duplicate_reply=false,drop_reply=false;
    bool submit(id holder,bytes bytes,io_completion done) noexcept {
        CAPSTAN_TRY {
            ++submissions; wire_bytes+=bytes.size();
            auto call=wire::decode_call(bytes);
            auto admission=authority.admit(call.header,holder);
            if(admission.code!=status::ok) {
                auto reply=wire::encode_reply(outcome::rejected,admission.code);
                wire_bytes+=reply.size();
                done({outcome::completed,std::move(reply)});
                return true;
            }
            CAPSTAN_TRY { queue.emplace(admission.pin,queued{std::move(call),std::move(done)}); }
            CAPSTAN_CATCH_ALL { CHECK(authority.dispatch_failed(admission.pin)==status::ok); return false; }
            return true;
        } CAPSTAN_CATCH_ALL { return false; }
    }
    void run_one() {
        host_authority::operation op;
        CHECK(authority.start_next(op));
        auto q=std::move(queue.at(op.pin)); queue.erase(op.pin);
        bytes reply;
        CAPSTAN_TRY { reply=wire::encode_reply(outcome::completed,status::ok,{resource->dispatch(q.call.header.operation,q.call.header.capability,q.call.args)}); }
        CAPSTAN_CATCH_ALL { reply=wire::encode_reply(outcome::completed,status::invalid); }
        CHECK(authority.complete(op.pin)==status::ok);
        if(authority.inspect().gate==host_authority::phase::dead) resource.reset();
        wire_bytes+=reply.size();
        auto copy=q.done;
        if(drop_reply) q.done({outcome::unknown,{}});
        else q.done({outcome::completed,reply});
        if(duplicate_reply) copy({outcome::completed,std::move(reply)});
    }
};
struct loopback_endpoint : io_endpoint {
    loopback_host& host; id holder;
    bool fail_submit=false;
    loopback_endpoint(loopback_host& h,id client):host(h),holder(client) {}
    bool submit(bytes request,io_completion done) noexcept override {
        if(fail_submit) return false;
        return host.submit(holder,std::move(request),std::move(done));
    }
};
grant make_grant(id holder,access_mode mode) { grant g;g.key={1,2,3};g.holder=holder;g.grant_id=holder;g.mode=mode;g.secret[0]=static_cast<unsigned char>(holder);return g; }
std::vector<grant> install(loopback_host& h,directory_entry& d,std::vector<grant> target) {
    transition t; acknowledgement ack; std::vector<grant> result;
    CHECK(d.begin(std::move(target),false,t)==status::ok);
    CHECK(h.authority.set_authority(t)==status::ok);
    CHECK(h.authority.ack(t.tx,ack)==status::ok);
    CHECK(d.receive(ack)==status::ok && d.take_grants(result)==status::ok);
    return result;
}
int main() {
    loopback_host host; directory_entry directory({1,2,3},9);
    loopback_endpoint a(host,1),b(host,2);
    auto grants=install(host,directory,{make_grant(1,access_mode::shared),make_grant(2,access_mode::shared)});
    read_capability reader_a(grants[0]),reader_b(grants[1]);
    auto reader_copy=reader_a;
    CHECK(host.submissions==0); // A local read copy never creates remote grant/release.
    summary_t sa,sb; int completions=0;
    start_call(a,reader_copy.summary(),[&](wire::reply r){sa=typed_call<summary_t>::decode(r);++completions;});
    start_call(b,reader_b.summary(),[&](wire::reply r){sb=typed_call<summary_t>::decode(r);++completions;});
    CHECK(host.authority.inspect().active==2 && completions==0);
    host.run_one();host.run_one();
    CHECK(sa==sb && sa.count==128 && completions==2);
    auto forged=wire::encode_call({grants[0],400,method::add_range},{wire::value::unsigned_integer(0),wire::value::unsigned_integer(1),wire::value::unsigned_integer(2)});
    start_io(a,std::move(forged),[&](io_result r){CHECK(wire::decode_reply(r.payload).code==status::forbidden);++completions;});
    CHECK(completions==3 && host.authority.inspect().active==0);
    auto old=grants;
    grants=install(host,directory,{make_grant(1,access_mode::exclusive)});
    start_call(b,reader_b.version(),[&](wire::reply r){CHECK(r.state==outcome::rejected && r.code==status::stale);++completions;});
    CHECK(completions==4);
    exclusive_capability writer(grants[0]);
    host.duplicate_reply=true;
    start_call(a,writer.add_range(0,16,1),[&](wire::reply r){CHECK(typed_call<std::uint64_t>::decode(r)==1);++completions;});
    bool busy=false;
    busy=rejects<capability_busy>([&]{ auto forbidden=writer.add_range(0,1,1); });
    CHECK(busy && host.authority.inspect().active==1);
    busy=false;
    busy=rejects<capability_busy>([&]{ auto forbidden=writer.version(); });
    CHECK(busy);
    host.run_one();
    CHECK(completions==5); // Duplicate bytes did not cause a second FF completion.
    start_call(a,writer.summary(),[&](wire::reply r){auto s=typed_call<summary_t>::decode(r);CHECK(s.sum==sa.sum+16 && s.version==1);++completions;});
    busy=false;
    busy=rejects<capability_busy>([&]{ auto forbidden=writer.add_range(0,1,1); });
    CHECK(busy); // Real read borrow covers the whole delayed interval.
    host.run_one();
    CHECK(completions==6);
    a.fail_submit=true;
    start_call(a,writer.add_range(0,1,9),[&](wire::reply r){CHECK(r.state==outcome::not_submitted);++completions;});
    CHECK(completions==7 && host.authority.inspect().active==0);
    a.fail_submit=false;
    host.drop_reply=true;
    start_call(a,writer.add_range(0,1,9),[&](wire::reply r){CHECK(r.state==outcome::unknown);++completions;});
    CHECK(host.authority.inspect().active==1);
    host.run_one();
    CHECK(completions==8 && host.authority.inspect().active==0);
    host.drop_reply=false;
    start_call(a,writer.version(),[&](wire::reply r){CHECK(typed_call<std::uint64_t>::decode(r)==2);++completions;});
    host.run_one(); CHECK(completions==9); // Lost response did not retry write.
    int reentrant=0;
    start_call(a,writer.add_range(0,0,0),[&](wire::reply r){
        CHECK(typed_call<std::uint64_t>::decode(r)==3); ++reentrant;
        start_call(a,writer.add_range(0,0,0),[&](wire::reply next){CHECK(typed_call<std::uint64_t>::decode(next)==4);++reentrant;});
    });
    host.run_one();host.run_one(); CHECK(reentrant==2);
    detachable_endpoint timed(a);
    int timed_out=0;
    start_call(timed,writer.add_range(0,1,2),[&](wire::reply r){CHECK(r.state==outcome::unknown);++timed_out;});
    CHECK(timed.timeout_pending() && timed_out==1 && host.authority.inspect().active==1);
    busy=false;
    busy=rejects<capability_busy>([&]{auto premature=writer.add_range(0,1,1);});
    CHECK(busy); // Detached underlying operation still retains the real Lease.
    transition handoff; acknowledgement handoff_ack;
    CHECK(directory.begin({make_grant(2,access_mode::exclusive)},false,handoff)==status::ok);
    CHECK(host.authority.set_authority(handoff)==status::pending);
    CHECK(host.authority.inspect().epoch==2);
    host.run_one(); // Remote really ends now; late reply cannot touch old FF frame.
    CHECK(timed_out==1 && host.authority.inspect().active==0);
    {auto returned=writer.add_range(0,0,0);} // Underlying completion returned token; do not submit stale grant.
    CHECK(host.authority.ack(handoff.tx,handoff_ack)==status::ok);
    CHECK(directory.receive(handoff_ack)==status::ok && directory.take_grants(grants)==status::ok);
    start_call(a,writer.version(),[&](wire::reply r){CHECK(r.code==status::stale);++completions;});
    CHECK(completions==10);
    {
        exclusive_capability moved_from(grants[0]);
        exclusive_capability survivor(std::move(moved_from));
        start_call(b,survivor.add_range(0,1,3),[&](wire::reply r){CHECK(typed_call<std::uint64_t>::decode(r)==6);++completions;});
    } // Owning capability destroyed, in-flight frame still owns referent+Lease.
    CHECK(host.authority.inspect().active==1);
    transition destroy;
    CHECK(directory.begin({},true,destroy)==status::ok);
    CHECK(host.authority.set_authority(destroy)==status::pending && host.resource);
    host.run_one();
    CHECK(completions==11 && !host.resource && host.authority.inspect().gate==host_authority::phase::dead);
    acknowledgement ack; std::vector<grant> empty;
    CHECK(host.authority.ack(destroy.tx,ack)==status::ok);
    CHECK(directory.receive(ack)==status::ok && directory.take_grants(empty)==status::ok && empty.empty());
    std::cout << "loopback: actual codec/object binder/Lease/FF, shared, gate, write, transfer, duplicate, submit-fail, lost-response, destroy; wire_bytes=" << host.wire_bytes << '\n';
}
