#include <capstan/async/io.hpp>
#include "tests/check.hpp"
#include <atomic>
#include <cstdlib>
#include <new>
// Fault only one allocation of the requested size, after request setup. This
// tests the real FF init/submit path, not a simulated successful callback.
static std::atomic<std::size_t> fail_size{0};
#if CAPSTAN_HAS_EXCEPTIONS
void* operator new(std::size_t size) {
    auto expected=size;
    if(fail_size.compare_exchange_strong(expected,0)) throw std::bad_alloc();
    if(void* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
#else
// Match every successful allocation with the replacement delete, including
// objects constructed through the injected nothrow overload below.
void* operator new(std::size_t size) {
    if(void* p=std::malloc(size ? size : 1)) return p;
    std::abort();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t size,const std::nothrow_t&) noexcept {
    auto expected=size;if(fail_size.compare_exchange_strong(expected,0)) return nullptr;
    return std::malloc(size ? size : 1);
}
void operator delete(void* p,const std::nothrow_t&) noexcept { std::free(p); }
#endif
struct never_submit : capstan::io_endpoint {
    int submitted=0;
    bool submit(capstan::bytes,capstan::io_completion) noexcept override { ++submitted;return false; }
};
int main() {
    never_submit endpoint;
    int callbacks=0;
    for(auto size:{sizeof(std::shared_ptr<capstan::async_detail::state>),sizeof(capstan::io_result)}) {
        capstan::bytes request{1,2,3};
        capstan::io_completion done=[&](capstan::io_result r){CHECK(r.state==capstan::outcome::not_submitted);++callbacks;};
        fail_size.store(size);
        capstan::start_io(endpoint,std::move(request),std::move(done));
        CHECK(fail_size.load()==0);
    }
    CHECK(callbacks==2 && endpoint.submitted==0);
    std::cout<<"allocation: FF context holder failure and response allocation failure produce NotSubmitted and zero transport submissions\n";
}
