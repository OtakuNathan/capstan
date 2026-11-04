#include <capstan/authority/host.hpp>
#include <capstan/authority/directory.hpp>
#include <thread>
#include "tests/check.hpp"
using namespace capstan;
grant g(id holder,access_mode m) { grant v; v.grant_id=holder; v.holder=holder; v.mode=m; v.secret[0]=static_cast<unsigned char>(holder); return v; }
int main() {
    object_key key{1,2,3};
    host_authority host(key,9);
    directory_entry table(key,9);
    transition t;
    CHECK(table.begin({g(1,access_mode::shared),g(2,access_mode::shared)},false,t)==status::ok);
    std::vector<grant> clients;
    CHECK(table.take_grants(clients)==status::pending);
    CHECK(host.set_authority(t)==status::ok);
    acknowledgement ack;
    CHECK(host.ack(t.tx,ack)==status::ok);
    auto wrong=ack; ++wrong.installed.next_epoch;
    CHECK(table.receive(wrong)==status::conflict && table.epoch()==0);
    // Drop the first ACK, retry the same immutable transaction.
    transition retry;
    CHECK(table.retry(retry)==status::ok && retry==t);
    CHECK(host.set_authority(retry)==status::ok);
    CHECK(table.receive(ack)==status::ok);
    CHECK(table.receive(ack)==status::duplicate);
    CHECK(table.take_grants(clients)==status::ok && clients.size()==2);
    CHECK(table.take_grants(clients)==status::pending);
    auto old=t;
    auto conflict=t; conflict.grants[0].secret[1]=8;
    CHECK(host.set_authority(conflict)==status::conflict);
    CHECK(host.admit({clients[0],1,method::add_range},1).code==status::forbidden);
    auto forged=clients[0]; forged.mode=access_mode::exclusive;
    CHECK(host.admit({forged,1,method::add_range},1).code==status::bad_grant);
    CHECK(host.admit({clients[0],1,method::version},2).code==status::bad_grant);
    auto a=host.admit({clients[0],1,method::version},1);
    auto b=host.admit({clients[1],1,method::summary},2);
    CHECK(a.code==status::ok && b.code==status::ok);
    CHECK(host.admit({clients[0],1,method::version},1).code==status::duplicate);
    host_authority::operation executing;
    CHECK(host.start_next(executing) && executing.pin==a.pin);
    CHECK(!host.start_next(executing));
    CHECK(table.begin({g(3,access_mode::exclusive)},false,t)==status::ok);
    CHECK(host.set_authority(t)==status::pending);
    CHECK(host.inspect().active==2 && host.inspect().epoch==1);
    CHECK(host.admit({clients[1],2,method::version},2).code==status::closed);
    CHECK(host.complete(b.pin)==status::invalid); // Queued is still active.
    CHECK(host.complete(a.pin)==status::ok);
    CHECK(host.inspect().epoch==1 && host.inspect().active==1);
    CHECK(host.ack(t.tx,ack)==status::pending);
    CHECK(host.start_next(executing) && executing.pin==b.pin);
    CHECK(host.complete(b.pin)==status::ok);
    CHECK(host.ack(t.tx,ack)==status::ok);
    CHECK(table.receive(ack)==status::ok);
    CHECK(table.take_grants(clients)==status::ok);
    CHECK(host.set_authority(old)==status::ok && host.inspect().epoch==2);
    CHECK(table.receive({old})==status::conflict && table.epoch()==2);
    CHECK(host.admit({old.grants[0],4,method::version},1).code==status::stale);
    a=host.admit({clients[0],1,method::add_range},3);
    b=host.admit({clients[0],2,method::add_range},3);
    CHECK(a.code==status::ok && b.code==status::ok);
    CHECK(host.start_next(executing) && executing.pin==a.pin);
    CHECK(!host.start_next(executing));
    // Client timeout/drop has no Host event and cannot decrement active.
    CHECK(host.inspect().active==2);
    CHECK(table.begin({},true,t)==status::ok);
    CHECK(host.set_authority(t)==status::pending);
    CHECK(host.complete(a.pin)==status::ok);
    CHECK(host.inspect().gate==host_authority::phase::closing);
    CHECK(host.start_next(executing));
    CHECK(host.complete(b.pin)==status::ok);
    CHECK(host.inspect().gate==host_authority::phase::dead && host.inspect().active==0);
    CHECK(host.admit({clients[0],3,method::version},3).code==status::dead);
    CHECK(host.ack(t.tx,ack)==status::ok);
    CHECK(table.receive(ack)==status::ok);
    CHECK(table.take_grants(clients)==status::ok && clients.empty());
    CHECK(table.begin({},false,t)==status::dead);
    auto bad=old; bad.control_session=10;
    CHECK(host.set_authority(bad)==status::wrong_control);
    host_authority reincarnated({1,8,3},9);
    CHECK(reincarnated.set_authority(old)==status::wrong_object);
    // Exhausted dedup history fails closed; dispatch failure removes only pins.
    host_authority bounded(key,9,1,2);
    CHECK(bounded.set_authority(old)==status::ok);
    a=bounded.admit({old.grants[0],1,method::version},1);
    CHECK(a.code==status::ok);
    CHECK(bounded.admit({old.grants[0],2,method::version},1).code==status::busy);
    CHECK(bounded.dispatch_failed(a.pin)==status::ok);
    CHECK(bounded.admit({old.grants[0],1,method::version},1).code==status::duplicate);
    a=bounded.admit({old.grants[0],2,method::version},1);
    CHECK(bounded.dispatch_failed(a.pin)==status::ok);
    CHECK(bounded.admit({old.grants[0],3,method::version},1).code==status::exhausted);
    // Close vs Admit races: accepted work MUST keep the old epoch installed.
    for(unsigned n=0;n<200;++n) {
        host_authority h(key,9);
        CHECK(h.set_authority(old)==status::ok);
        auto next=old; next.tx=2; next.expected_epoch=1; next.next_epoch=2; next.grants.clear();
        host_authority::admission result{status::invalid};
        std::thread admission([&]{result=h.admit({old.grants[0],1,method::version},1);});
        std::thread close([&]{auto s=h.set_authority(next); CHECK(s==status::ok || s==status::pending);});
        admission.join(); close.join();
        if(result.code==status::ok) {
            CHECK(h.inspect().epoch==1 && h.inspect().active==1);
            CHECK(h.start_next(executing)); CHECK(h.complete(result.pin)==status::ok);
        } else CHECK(result.code==status::closed || result.code==status::stale);
        CHECK(h.inspect().epoch==2 && h.inspect().active==0);
    }
    std::cout << "authority: grants, effects, dedup, ACK retry, queue drain, tombstone, bounds and 200 admission/close races passed\n";
}
