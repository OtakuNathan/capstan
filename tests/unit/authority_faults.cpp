#include <capstan/authority/host.hpp>
#include <capstan/authority/directory.hpp>
#include <limits>
#include "tests/check.hpp"
using namespace capstan;
namespace {
const object_key key{17,19,23};
const id controller=29;
grant make_grant(id holder, access_mode mode=access_mode::shared) {
    grant g;
    g.key=key; g.epoch=1; g.grant_id=holder; g.holder=holder; g.mode=mode;
    // Deterministic test tokens only; runtime obtains opaque OS-random bytes.
    g.secret[0]=static_cast<unsigned char>(holder);
    return g;
}
transition initial() { return {key,controller,1,0,1,false,{make_grant(1),make_grant(2)}}; }
void test_rejected_targets_leave_state_unchanged() {
    auto base=initial();
    std::vector<transition> invalid;
    auto t=base; t.grants[0].mode=access_mode::exclusive; invalid.push_back(t);
    t=base; t.grants[0].mode=static_cast<access_mode>(999); invalid.push_back(t);
    t=base; t.grants[1].grant_id=t.grants[0].grant_id; invalid.push_back(t);
    t=base; t.grants[1].holder=t.grants[0].holder; invalid.push_back(t);
    t=base; t.grants[1].secret=t.grants[0].secret; invalid.push_back(t);
    t=base; t.grants[0].secret={}; invalid.push_back(t);
    t=base; t.grants[0].holder=0; invalid.push_back(t);
    t=base; t.grants[0].grant_id=0; invalid.push_back(t);
    t=base; t.destroy=true; invalid.push_back(t);
    for(const auto& bad:invalid) {
        host_authority h(key,controller);
        CHECK(h.set_authority(bad)==status::invalid);
        CHECK(h.inspect().epoch==0 && h.inspect().gate==host_authority::phase::open);
        CHECK(h.set_authority(base)==status::ok);
        directory_entry d(key,controller);
        transition out;
        CHECK(d.begin(bad.grants,bad.destroy,out)==status::invalid);
        // Rejection does not consume tx=1 or enter Transitioning.
        CHECK(d.begin(base.grants,false,out)==status::ok && out.tx==1);
    }
    host_authority h(key,controller);
    t=base; ++t.grants[0].epoch; CHECK(h.set_authority(t)==status::invalid);
    t=base; ++t.grants[0].key.object; CHECK(h.set_authority(t)==status::invalid);
    t=base; t.tx=0; CHECK(h.set_authority(t)==status::invalid);
    t=base; t.grants.resize(65); CHECK(h.set_authority(t)==status::exhausted);
    directory_entry d(key,controller);
    transition out;
    CHECK(d.begin(t.grants,false,out)==status::exhausted);
    CHECK(d.begin(base.grants,false,out)==status::ok && out.tx==1);
}
void test_close_retry_reorder_and_exact_ack() {
    host_authority h(key,controller);
    directory_entry d(key,controller);
    transition t1;
    CHECK(d.begin(initial().grants,false,t1)==status::ok);
    CHECK(h.set_authority(t1)==status::ok);
    acknowledgement a1;
    CHECK(h.ack(t1.tx,a1)==status::ok);
    CHECK(d.receive(a1)==status::ok);
    transition ignored;
    CHECK(d.begin({},false,ignored)==status::busy); // Undelivered grants retained.
    std::vector<grant> clients;
    CHECK(d.take_grants(clients)==status::ok);
    auto pin=h.admit({clients[0],7,method::summary},clients[0].holder);
    CHECK(pin.code==status::ok);
    transition t2;
    CHECK(d.begin({make_grant(3,access_mode::exclusive)},false,t2)==status::ok);
    CHECK(h.set_authority(t2)==status::pending);
    CHECK(h.set_authority(t2)==status::pending);
    auto bad=t2; ++bad.tx; CHECK(h.set_authority(bad)==status::busy);
    bad=t2; bad.grants.clear(); CHECK(h.set_authority(bad)==status::conflict);
    CHECK(h.set_authority(t1)==status::ok); // Old retry cannot reopen gate.
    CHECK(h.inspect().gate==host_authority::phase::closing && h.inspect().epoch==1);
    CHECK(d.receive(a1)==status::conflict);
    CHECK(d.take_grants(clients)==status::pending);
    acknowledgement ack;
    CHECK(h.ack(t2.tx,ack)==status::pending);
    CHECK(h.ack(999,ack)==status::invalid);
    host_authority::operation op;
    CHECK(h.start_next(op));
    CHECK(h.dispatch_failed(pin.pin)==status::invalid); // Running cannot roll back.
    CHECK(h.complete(pin.pin)==status::ok);
    CHECK(h.complete(pin.pin)==status::invalid);
    CHECK(h.ack(t2.tx,ack)==status::ok);
    for(unsigned which=0;which<7;++which) {
        auto wrong=ack;
        switch(which) {
            case 0: ++wrong.installed.control_session; break;
            case 1: ++wrong.installed.key.incarnation; break;
            case 2: ++wrong.installed.tx; break;
            case 3: ++wrong.installed.expected_epoch; break;
            case 4: ++wrong.installed.next_epoch; break;
            case 5: wrong.installed.destroy=true; break;
            case 6: wrong.installed.grants[0].secret[1]=1; break;
        }
        CHECK(d.receive(wrong)==status::conflict && d.epoch()==1);
    }
    transition retry;
    CHECK(d.retry(retry)==status::ok && retry==t2);
    CHECK(h.set_authority(retry)==status::ok);
    CHECK(d.receive(ack)==status::ok && d.epoch()==2);
    CHECK(d.receive(ack)==status::duplicate);
    CHECK(d.take_grants(clients)==status::ok && clients.size()==1);
    CHECK(d.take_grants(clients)==status::pending);
    CHECK(h.set_authority(t1)==status::ok && h.inspect().epoch==2);
    bad=t1; bad.tx=100;
    CHECK(h.set_authority(bad)==status::stale);
}
void test_admission_fields_and_bounds() {
    auto t=initial();
    host_authority h(key,controller,2,3);
    CHECK(h.set_authority(t)==status::ok);
    auto g=t.grants[0];
    for(unsigned which=0;which<7;++which) {
        auto bad=g;
        switch(which) {
            case 0: ++bad.key.cluster; break;
            case 1: ++bad.key.incarnation; break;
            case 2: ++bad.key.object; break;
            case 3: ++bad.grant_id; break;
            case 4: ++bad.holder; break;
            case 5: bad.mode=access_mode::exclusive; break;
            case 6: ++bad.secret[8]; break;
        }
        auto result=h.admit({bad,1,method::version},g.holder);
        CHECK(result.code==(which<3 ? status::wrong_object : status::bad_grant));
        CHECK(h.inspect().active==0);
    }
    CHECK(h.admit({g,0,method::version},g.holder).code==status::invalid);
    CHECK(h.admit({g,1,static_cast<method>(99)},g.holder).code==status::unknown_method);
    CHECK(h.admit({g,1,method::add_range},g.holder).code==status::forbidden);
    auto p1=h.admit({g,1,method::version},g.holder);
    auto p2=h.admit({g,2,method::version},g.holder);
    CHECK(p1.code==status::ok && p2.code==status::ok);
    CHECK(h.admit({g,3,method::version},g.holder).code==status::busy);
    CHECK(h.complete(p1.pin)==status::invalid); // Accepted is not execution ended.
    CHECK(h.dispatch_failed(p2.pin)==status::ok);
    CHECK(h.admit({g,2,method::version},g.holder).code==status::duplicate);
    auto p3=h.admit({g,std::numeric_limits<id>::max(),method::version},g.holder);
    CHECK(p3.code==status::ok); // Identity accepted as-is; never incremented by Host.
    CHECK(h.dispatch_failed(p3.pin)==status::ok);
    CHECK(h.admit({g,3,method::version},g.holder).code==status::exhausted);
    CHECK(h.inspect().active==1);
}
void test_control_history_tombstone_and_no_wrap() {
    auto t=initial();
    host_authority h(key,controller,8,2);
    auto bad=t; ++bad.control_session;
    CHECK(h.set_authority(bad)==status::wrong_control);
    bad=t; ++bad.key.cluster; CHECK(h.set_authority(bad)==status::wrong_object);
    bad=t; bad.expected_epoch=std::numeric_limits<id>::max(); bad.next_epoch=0;
    CHECK(h.set_authority(bad)==status::invalid);
    bad=t; bad.expected_epoch=std::numeric_limits<id>::max()-1; bad.next_epoch=0;
    CHECK(h.set_authority(bad)==status::invalid);
    CHECK(h.set_authority(t)==status::ok);
    auto t2=t; t2.tx=2; t2.expected_epoch=1; t2.next_epoch=2; t2.grants.clear();
    CHECK(h.set_authority(t2)==status::ok);
    auto t3=t2; t3.tx=3; t3.expected_epoch=2; t3.next_epoch=3;
    CHECK(h.set_authority(t3)==status::exhausted);
    CHECK(h.inspect().epoch==2 && h.inspect().gate==host_authority::phase::open);
    CHECK(h.set_authority(t)==status::ok); // Recorded retries still work at bound.
    acknowledgement a;
    CHECK(h.ack(t.tx,a)==status::ok && a.installed==t);
    host_authority dying(key,controller);
    CHECK(dying.set_authority(t)==status::ok);
    t2.destroy=true;
    CHECK(dying.set_authority(t2)==status::ok);
    CHECK(dying.inspect().gate==host_authority::phase::dead);
    CHECK(dying.set_authority(t2)==status::ok);
    CHECK(dying.set_authority(t)==status::ok);
    CHECK(dying.inspect().gate==host_authority::phase::dead && dying.inspect().epoch==2);
    CHECK(dying.set_authority(t3)==status::dead);
    CHECK(dying.admit({t.grants[0],10,method::version},1).code==status::dead);
}
}
int main() {
    test_rejected_targets_leave_state_unchanged();
    test_close_retry_reorder_and_exact_ack();
    test_admission_fields_and_bounds();
    test_control_history_tombstone_and_no_wrap();
    std::cout << "authority faults: invalid-target rollback, delayed/duplicate/exact ACK, all grant fields, controls, bounds, no-wrap and tombstone passed\n";
}
