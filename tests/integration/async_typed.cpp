#include <capstan/access/capability.hpp>
#include <capstan/access/clipboard.hpp>
#include <capstan/async/detachable.hpp>
#include "tests/check.hpp"
using namespace capstan;
struct endpoint:io_endpoint {
    io_completion pending;
    bytes request;
    bool submit(bytes r,io_completion cb) noexcept override {if(pending)return false;request=std::move(r);pending=std::move(cb);return true;}
    void complete(bytes response){auto cb=std::exchange(pending,nullptr);cb({outcome::completed,std::move(response)});cb({outcome::completed,{}});}
};
int main(){
    for(unsigned o=0;o<=unsigned(outcome::unknown);++o) for(unsigned s=0;s<=unsigned(status::superseded);++s){
        outcome decoded_o;status decoded_s;
        CHECK(decode_rpc_error(make_rpc_error(outcome(o),status(s)),decoded_o,decoded_s));
        CHECK(decoded_o==outcome(o) && decoded_s==status(s));
    }
    grant g;g.key={1,2,3};g.epoch=1;g.grant_id=1;g.holder=1;g.secret[0]=1;g.mode=access_mode::exclusive;
    auto owner=std::make_shared<exclusive_capability>(g);endpoint ep;
    async_exclusive_capability view(owner,ep);
    int called=0;
    auto call=view.add_range(0,1,2);
    CHECK(!owner->try_add_range(0,1,1));
    dynabridge::ff::start_invocation(std::move(call),[&](flux_foundry::result_t<id,dynabridge::async_error> r) noexcept {
        CHECK(r.has_value() && r.value()==3);++called;
        CHECK(bool(owner->try_add_range(0,1,1))); // Old operation borrow released before continuation.
    });
    ep.complete(wire::encode_reply(outcome::completed,status::ok,{wire::u(3)}));CHECK(called==1);
    detachable_endpoint timeout(ep);
    async_exclusive_capability timed(owner,timeout);
    dynabridge::ff::start_invocation(timed.add_range(0,1,2),[&](flux_foundry::result_t<id,dynabridge::async_error> r) noexcept {
        outcome state;status code;CHECK(!r.has_value() && decode_rpc_error(r.error(),state,code) && state==outcome::unknown);++called;
        CHECK(!owner->try_add_range(0,1,1));
    });
    CHECK(timeout.timeout_pending());CHECK(called==2);
    ep.complete(wire::encode_reply(outcome::completed,status::ok,{wire::u(4)}));CHECK(called==2);
    CHECK(bool(owner->try_add_range(0,1,1)));
    dynabridge::ff::start_invocation(view.version(),[&](flux_foundry::result_t<id,dynabridge::async_error> r) noexcept {
        outcome state;status code;CHECK(!r.has_value() && decode_rpc_error(r.error(),state,code) && state==outcome::unknown);++called;
    });
    ep.complete({1,2});CHECK(called==3);
    auto shared_endpoint=std::shared_ptr<io_endpoint>(&ep,[](io_endpoint*) {});
    auto snapshot_call=capstan::copy(shared_endpoint,*owner);
    CHECK(!owner->try_add_range(0,1,1));
    dynabridge::ff::start_invocation(std::move(snapshot_call),[&](flux_foundry::result_t<migration::snapshot_ref,dynabridge::async_error> r) noexcept {
        CHECK(r.has_value() && r.value().source==g.key);++called;
        CHECK(bool(owner->try_add_range(0,1,1)));
    });
    wire::codec::request routed,copy;
    CHECK(wire::try_decode(ep.request,routed) && routed.method==migration::route_command);
    CHECK(wire::try_decode(wire::binary(wire::string(routed.args[3])),copy) && copy.method==migration::copy_command);
    migration::snapshot_ref ref;ref.source=g.key;ref.holder=g.holder;ref.host=1;ref.instance=2;ref.sequence=wire::number(copy.args[1]);ref.size=128;
    ep.complete(wire::encode_reply(outcome::completed,status::ok,{wire::value::string(migration::encode_snapshot_ref(ref))}));
    CHECK(called==4);

}
