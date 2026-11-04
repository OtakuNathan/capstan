#include <capstan/authority/host.hpp>
#include <capstan/authority/directory.hpp>
#include "tests/check.hpp"
#include <atomic>
#include <cstdlib>
#include <new>
namespace {
std::atomic<int> fail_after{-1};
}
void* operator new(std::size_t size) {
    int n=fail_after.load();
    if(n>=0) {
        if(n==0) { fail_after=-1;throw std::bad_alloc(); }
        fail_after=n-1;
    }
    if(auto* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
using namespace capstan;
namespace {
const object_key key{1,2,3};
grant g(id holder,id epoch) { grant x;x.key=key;x.epoch=epoch;x.grant_id=holder;x.holder=holder;x.secret[0]=static_cast<unsigned char>(holder);return x; }
transition first() { return {key,4,1,0,1,false,{g(1,1)}}; }
transition next() { return {key,4,2,1,2,false,{g(2,2)}}; }
template<class F> void allocation_failure(F f) {
    bool caught=false;fail_after=0;
    try {f();} catch(const std::bad_alloc&) {caught=true;}
    fail_after=-1;CHECK(caught);
}
void allocation_free_install(bool completing) {
    host_authority host(key,4);
    CHECK(host.set_authority(first())==status::ok);
    auto p=host.admit({g(1,1),1,method::version},1);CHECK(p.code==status::ok);
    host_authority::operation op;
    if(completing) CHECK(host.start_next(op));
    auto t=next();CHECK(host.set_authority(t)==status::pending);
    // No allocation is allowed after the final pin is removed. The hook would
    // throw on the very next allocation; installation must not trigger it.
    fail_after=0;
    status result=completing ? host.complete(p.pin) : host.dispatch_failed(p.pin);
    const bool unallocated=fail_after.load()==0;
    fail_after=-1;
    CHECK(result==status::ok && unallocated);
    auto state=host.inspect();
    CHECK(state.epoch==2 && state.active==0 && state.gate==host_authority::phase::open);
    CHECK(state.grants==next().grants);
    acknowledgement ack;CHECK(host.ack(t.tx,ack)==status::ok && ack.installed==t);
    CHECK(host.complete(p.pin)==status::invalid);
    CHECK(host.set_authority(t)==status::ok);
}
void failed_transition_staging() {
    // Target staging, transaction map node and immutable history payload each
    // allocate before Close. Any one may fail without changing observable state.
    for(int allocation=0;allocation<3;++allocation) {
        host_authority host(key,4);
        CHECK(host.set_authority(first())==status::ok);
        auto t=next();bool caught=false;fail_after=allocation;
        try {host.set_authority(t);} catch(const std::bad_alloc&) {caught=true;}
        fail_after=-1;CHECK(caught);
        auto state=host.inspect();
        CHECK(state.epoch==1 && state.active==0 && state.gate==host_authority::phase::open);
        CHECK(state.grants==first().grants);
        acknowledgement ack;CHECK(host.ack(t.tx,ack)==status::invalid);
        CHECK(host.set_authority(t)==status::ok && host.inspect().epoch==2);
    }
}
void failed_directory_and_admit() {
    directory_entry directory(key,4);transition out;
    auto grants=first().grants;
    allocation_failure([&]{directory.begin(std::move(grants),false,out);});
    CHECK(directory.epoch()==0);
    CHECK(directory.begin(first().grants,false,out)==status::ok && out.tx==1);
    host_authority host(key,4);
    CHECK(host.set_authority(first())==status::ok);
    allocation_failure([&]{host.admit({g(1,1),1,method::version},1);});
    CHECK(host.inspect().active==0);
    auto admitted=host.admit({g(1,1),1,method::version},1);
    CHECK(admitted.code==status::ok); // No ghost dedup entry on allocation failure.
    CHECK(host.dispatch_failed(admitted.pin)==status::ok);
}
}
int main() {
    allocation_free_install(true);allocation_free_install(false);failed_transition_staging();failed_directory_and_admit();
    std::cout << "authority allocation: staging failures preserve old Open epoch; complete/dispatch_failed install without allocation; Directory/admit rollback passed\n";
}
