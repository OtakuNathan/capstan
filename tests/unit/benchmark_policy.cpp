#include <capstan/authority/host.hpp>
#include "tests/check.hpp"
using namespace capstan;
namespace {
const object_key key{1,2,3};
grant sample() { grant g; g.key=key;g.epoch=1;g.grant_id=1;g.holder=1;g.secret[0]=1;return g; }
transition initial() { return {key,4,1,0,1,false,{sample()}}; }
void complete(host_authority& h,id pin) {
    host_authority::operation running;
    CHECK(h.start_next(running) && running.pin==pin);
    CHECK(h.complete(pin)==status::ok);
}
void checked_still_rejects() {
    host_authority checked(key,4,host_authority::benchmark_validation::checked);
    host_authority defaults(key,4);
    for(auto* h : {&checked,&defaults}) {
        CHECK(h->set_authority(initial())==status::ok);
        auto g=sample();
        CHECK(h->admit({g,1,method::add_range},1).code==status::forbidden);
        g.secret[1]=1;
        CHECK(h->admit({g,1,method::version},1).code==status::bad_grant);
        CHECK(h->inspect().active==0);
    }
}
void omitted_checks_retain_lifetime() {
    host_authority baseline(key,4,host_authority::benchmark_validation::omit_grant_checks,2,3);
    CHECK(baseline.set_authority(initial())==status::ok);
    auto forged=sample();forged.key.incarnation=99;forged.epoch=99;forged.holder=99;forged.secret[0]=99;
    auto first=baseline.admit({forged,1,method::add_range},1);
    CHECK(first.code==status::ok); // Explicit benchmark-only auth bypass.
    auto second=baseline.admit({forged,2,method::version},1);
    CHECK(second.code==status::ok && baseline.inspect().active==2);
    CHECK(baseline.admit({forged,2,method::version},1).code==status::duplicate);
    CHECK(baseline.admit({forged,3,method::version},1).code==status::busy);
    CHECK(baseline.admit({forged,3,static_cast<method>(999)},1).code==status::unknown_method);
    auto close=initial();close.tx=2;close.expected_epoch=1;close.next_epoch=2;close.grants.clear();
    CHECK(baseline.set_authority(close)==status::pending);
    CHECK(baseline.inspect().epoch==1);
    CHECK(baseline.admit({forged,3,method::version},1).code==status::closed);
    complete(baseline,first.pin);
    CHECK(baseline.inspect().epoch==1 && baseline.inspect().active==1);
    complete(baseline,second.pin);
    CHECK(baseline.inspect().epoch==2 && baseline.inspect().active==0);
    close.tx=3;close.expected_epoch=2;close.next_epoch=3;close.destroy=true;
    CHECK(baseline.set_authority(close)==status::ok);
    CHECK(baseline.admit({forged,3,method::version},1).code==status::dead);
    CHECK(baseline.inspect().active==0);
}
}
int main() {
    checked_still_rejects();omitted_checks_retain_lifetime();
    std::cout << "benchmark policy: default checked; explicit omission retains whitelist, pins, gate, lane, dedup, bounds and drain\n";
}
