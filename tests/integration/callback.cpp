#include <capstan/error_policy.hpp>
#include <capstan/access/capability.hpp>
#include <capstan/async/detachable.hpp>
#include <atomic>
#include "tests/check.hpp"
#include <thread>
using namespace capstan;
struct manual : io_endpoint {
    io_completion pending;
    bytes sent;
    bool immediate=false;
    bool submit(bytes request,io_completion done) noexcept override {
        if(pending) return false;
        sent=std::move(request);
        if(immediate) done({outcome::completed,wire::encode_reply(outcome::completed,status::ok)});
        else pending=std::move(done);
        return true;
    }
    void finish() {
        auto cb=std::exchange(pending,nullptr);
        cb({outcome::completed,wire::encode_reply(outcome::completed,status::ok)});
        cb({outcome::completed,wire::encode_reply(outcome::completed,status::ok)});
    }
};
struct submit_race : io_endpoint {
    std::function<void()> during_submit;
    bool accepted;
    explicit submit_race(bool yes):accepted(yes) {}
    bool submit(bytes,io_completion done) noexcept override {
        during_submit();
        if(accepted) done({outcome::completed,{}});
        return accepted;
    }
};
int main() {
    manual endpoint;
    int finished=0;
    auto pin=std::make_shared<int>(7);
    std::weak_ptr<int> weak=pin;
    start_io(endpoint,{1,2,3},[&](io_result r){CHECK(r.state==outcome::completed);CHECK(weak.expired());++finished;},std::move(pin));
    CHECK(!weak.expired() && finished==0);
    std::thread worker([&]{endpoint.finish();});worker.join();
    CHECK(finished==1 && weak.expired());
    {
        manual network;
        auto captured=std::make_shared<int>(0);
        std::weak_ptr<int> lifetime=captured;
        start_io(network,{1},[captured](io_result r){CHECK(r.state==outcome::completed);++*captured;});
        captured.reset();
        auto late=std::exchange(network.pending,nullptr);
        late({outcome::completed,{}});
        CHECK(lifetime.expired()); // Retained transport callback cannot retain the consumed user callback.
        late({outcome::completed,{}});
    }
    {
        manual network;
        detachable_endpoint deadline(network);
        auto captured=std::make_shared<int>(0);
        std::weak_ptr<int> lifetime=captured;
        CHECK(deadline.submit({1},[captured](io_result r){CHECK(r.state==outcome::completed);++*captured;}));
        captured.reset();network.finish();
        CHECK(lifetime.expired()); // The settled registration can remain alive, but its callback is consumed.
    }
    grant g;g.key={1,2,3};g.epoch=1;g.grant_id=1;g.holder=1;g.secret[0]=1;
    read_capability reader(g); auto copy=reader; read_capability moved_copy(std::move(copy));
    reader.close(endpoint,wire::command(2002),[&](io_result r){CHECK(wire::decode_reply(r.payload).code==status::ok);++finished;});
    bool closed=false;
    closed=rejects<capability_busy>([&]{auto call=moved_copy.version();});
    CHECK(closed && finished==1);
    endpoint.finish(); CHECK(finished==2);
    g.mode=access_mode::exclusive;
    exclusive_capability writer(g);
    bool duplicate=false;
    duplicate=rejects<std::logic_error>([&]{exclusive_capability second(g);});
    CHECK(duplicate);
    {
        auto pending=writer.version();
        bool busy=false;
        busy=!writer.try_close(endpoint,wire::command(2002),[](io_result){});
        CHECK(busy);
    }
    endpoint.immediate=true;
    writer.close(endpoint,wire::command(2002),[&](io_result r){CHECK(r.state==outcome::completed);++finished;});
    CHECK(finished==3);
    closed=false;
    closed=rejects<capability_busy>([&]{auto call=writer.add_range(0,1,1);});
    CHECK(closed);
    duplicate=false;
    duplicate=rejects<std::logic_error>([&]{exclusive_capability second(g);});
    CHECK(duplicate); // A closed but live owner cannot be rematerialized.
    {
        auto retained_grant=g;retained_grant.grant_id=2;retained_grant.secret[1]=2;
        manual network;
        {
            exclusive_capability first(retained_grant);
            exclusive_capability moved(std::move(first));
            bool spent=false;
            spent=rejects<capability_busy>([&]{auto call=first.version();});
            CHECK(spent);
            start_call(network,moved.add_range(0,1,1),[](wire::reply r){CHECK(r.state==outcome::completed);});
        }
        duplicate=false;
        duplicate=rejects<std::logic_error>([&]{exclusive_capability second(retained_grant);});
        CHECK(duplicate); // Operation still owns original local lineage.
        network.finish();
        exclusive_capability reconstructed(retained_grant); // Old state expired.
        auto read=reconstructed.version();
    }
    {
        std::vector<std::unique_ptr<exclusive_capability>> sessions;
        for(unsigned i=0;i<63;++i) {
            auto extra=g;extra.grant_id=1000+i;
            sessions.emplace_back(new exclusive_capability(extra));
        }
        auto extra=g;extra.grant_id=9999;
        bool full=false;
        full=rejects<std::length_error>([&]{exclusive_capability excess(extra);});
        CHECK(full);
        sessions.clear();
        exclusive_capability after_prune(extra);
    }
    {
        manual network;
        detachable_endpoint deadline(network);
        auto authority=std::make_shared<int>(9);
        std::weak_ptr<int> detached=authority;
        unsigned count=0;
        start_io(deadline,{1},[&](io_result r){CHECK(r.state==outcome::unknown && !detached.expired());++count;},std::move(authority));
        CHECK(deadline.timeout_pending() && count==1 && !detached.expired());
        network.finish();
        CHECK(count==1 && detached.expired());
    }
    for(bool accepted:{false,true}) {
        submit_race network(accepted);
        detachable_endpoint deadline(network);
        network.during_submit=[&]{CHECK(deadline.timeout_pending());};
        unsigned count=0;
        start_io(deadline,{1},[&](io_result r){
            CHECK(r.state==(accepted ? outcome::unknown : outcome::not_submitted));
            ++count;
        });
        CHECK(count==1);
    }
    for(unsigned i=0;i<100;++i) {
        manual network;
        detachable_endpoint deadline(network);
        std::atomic<unsigned> count{0};
        start_io(deadline,{1},[&](io_result r){CHECK(r.state==outcome::completed || r.state==outcome::unknown);++count;});
        auto reply=std::exchange(network.pending,nullptr);
        std::thread timer([&]{deadline.timeout_pending();});
        std::thread remote([&]{reply({outcome::completed,{}});});
        timer.join();remote.join();
        reply({outcome::completed,{}});
        CHECK(count.load()==1 && !deadline.timeout_pending());
    }
    std::cout<<"callback: worker completion, authority pin, late duplicate, shared-copy close gate, exclusive close gate, inline completion passed\n";
}
