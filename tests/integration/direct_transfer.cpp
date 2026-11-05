#include <capstan/access/transfer.hpp>
#include "src/apps/authority_replies.hpp"
#include "tests/check.hpp"
using namespace capstan;
struct loopback final:io_endpoint {
    host_authority host{{91,92,93},94};
    app::authority_replies replies{host};
    dataset_binding dataset{{91,92,93},4096};
    id caller=1;
    bool refuse=false,delay=false,drop=false;
    bytes delayed;
    io_completion delayed_done,business_done;
    request business;
    std::vector<wire::value> arguments;
    id pin=0;
    static bool mint(grant& g) {g.grant_id=100+g.epoch;g.secret[0]=static_cast<unsigned char>(g.epoch);return true;}
    bool submit(bytes b,io_completion done) noexcept override {
        if(refuse) return false;
        if(delay) {delayed=std::move(b);delayed_done=std::move(done);return true;}
        wire::codec::request decoded;CHECK(wire::try_decode(b,decoded));
        if(decoded.method==wire::query_transfer || decoded.method==wire::claim_transfer) {
            transfer_ref ref;CHECK(wire::try_decode_transfer_ref(wire::string(decoded.args[0]),ref));
            if(decoded.method==wire::query_transfer) {
                auto receipt=host.query_transfer(ref,caller);
                done({outcome::completed,wire::encode_reply(receipt.code==status::ok ? outcome::completed:outcome::rejected,receipt.code,{wire::value::string(wire::encode_transfer_receipt(receipt))})});
            } else {
                grant g;auto code=host.claim_transfer(ref,caller,g);
                done({outcome::completed,wire::encode_reply(code==status::ok ? outcome::completed:outcome::rejected,code,code==status::ok ? std::vector<wire::value>{wire::value::string(wire::encode_grant(g))}:std::vector<wire::value>{})});
            }
            return true;
        }
        if(decoded.method==wire::transfer_exclusive) {
            transfer_request request;CHECK(wire::try_decode_transfer_request(wire::string(decoded.args[0]),request));
            replies.transfer(request,caller,mint,[done=std::move(done),lost=drop](io_result r) mutable {
                if(lost) {auto copy=done;done({outcome::unknown,{}});copy({outcome::unknown,{}});}
                else done(std::move(r));
            });replies.flush();return true;
        }
        wire::call call;CHECK(wire::try_decode_call(b,call));business=call.header;arguments=std::move(call.args);
        auto admission=host.admit(business,caller);CHECK(admission.code==status::ok);
        pin=admission.pin;host_authority::operation op;CHECK(host.start_next(op));
        business_done=std::move(done);return true;
    }
    void drain() {
        wire::value value;CHECK(dataset.try_dispatch(business.operation,business.capability,arguments,value));
        CHECK(host.complete(pin)==status::ok);replies.flush();
        auto done=std::exchange(business_done,nullptr);done({outcome::completed,wire::encode_reply(outcome::completed,status::ok,{value})});
    }
    void deliver() {delay=false;auto b=std::move(delayed);auto done=std::exchange(delayed_done,nullptr);CHECK(submit(std::move(b),std::move(done)));}
};
int main() {
    loopback transport;
    grant initial{{91,92,93},1,1,1,access_mode::exclusive,{}};initial.secret[0]=1;
    CHECK(transport.host.set_authority({initial.key,94,1,0,1,false,{initial}})==status::ok);
    status code;auto source=exclusive_capability::try_create(initial,code);CHECK(source && code==status::ok);
    auto prepared=transfer_plan::prepare(*source,2,1,code);CHECK(prepared && !prepared->sealed());
    CHECK(source->try_version()); // Constructing a descriptor has no side effects.
    unsigned business_done=0,transfer_done=0;
    auto work=source->try_add_range(0,8,3);CHECK(work);
    start_call(transport,std::move(*work),std::function<void(wire::reply)>([&](wire::reply r){CHECK(r.code==status::ok);++business_done;}));
    CHECK(transport.host.inspect().active==1);
    prepared->start(transport,[&](io_result r){
        CHECK(r.state==outcome::completed);++transfer_done;
        auto reply=wire::decode_reply(r.payload);CHECK(reply.code==status::ok);
        CHECK(transport.host.inspect().active==0); // Callback outside mutex.
    });
    CHECK(prepared->sealed() && !source->try_version() && !source->try_add_range(0,1,1));
    CHECK(transfer_done==0 && transport.host.inspect().gate==host_authority::phase::closing);
    grant next;auto ref=prepared->request().reference();
    CHECK(transport.host.claim_transfer(ref,2,next)==status::pending);
    transport.drain();CHECK(business_done==1 && transfer_done==1);
    CHECK(!source->try_version()); // Old write_guard destructor did not reopen it.
    source.reset();prepared.reset();
    CHECK(!exclusive_capability::try_create(initial,code) && code==status::duplicate);
    CHECK(transport.host.claim_transfer(ref,2,next)==status::ok);
    unsigned queried=0;
    capstan::query_transfer(transport,ref,[&](transfer_query_result result){CHECK(result.code==status::ok && result.receipt.installed_epoch==2);++queried;});
    CHECK(queried==1);transport.caller=2;
    std::unique_ptr<exclusive_capability> recipient;
    capstan::claim_transfer(transport,ref,[&](transfer_claim_result result){CHECK(result.code==status::ok);recipient=std::move(result.capability);});CHECK(recipient);
    CHECK(!exclusive_capability::try_create(next,code) && code==status::duplicate);
    transport.caller=2;
    auto back=transfer_plan::prepare(*recipient,1,1,code);CHECK(back);
    transport.delay=true;unsigned unknown=0;
    back->start(transport,[&](io_result r){CHECK(r.state==outcome::unknown);++unknown;});
    CHECK(back->sealed() && !recipient->try_version());
    auto backref=back->request().reference();
    CHECK(transport.host.query_transfer(backref,2).code==status::not_seen);
    CHECK(!recipient->try_version()); // NotSeen is a snapshot, never a rollback.
    recipient.reset();transport.drop=true;transport.deliver();
    CHECK(unknown==1); // Deliberate duplicate transport completion is ignored by FF.
    CHECK(transport.host.query_transfer(backref,2).observation==transfer_observation::installed);
    transport.drop=false;unsigned retries=0;
    back->retry(transport,[&](io_result r){CHECK(wire::decode_reply(r.payload).code==status::ok);++retries;});
    CHECK(retries==1 && transport.host.inspect().epoch==3);
    CHECK(transport.host.claim_transfer(backref,1,next)==status::ok);
    auto again=exclusive_capability::try_create(next,code);CHECK(again);
    auto failed=transfer_plan::prepare(*again,2,2,code);CHECK(failed);
    transport.refuse=true;unsigned refusals=0;
    failed->start(transport,[&](io_result r){CHECK(r.state==outcome::not_submitted);++refusals;});
    CHECK(refusals==1 && failed->sealed() && !again->try_version());
    CHECK(transport.host.inspect().epoch==3); // Conservative local seal on attempt.
    again.reset();failed.reset();
    CHECK(!exclusive_capability::try_create(next,code) && code==status::duplicate);
}
