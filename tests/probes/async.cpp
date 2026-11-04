#include <capstan/async/errors.hpp>
#include <flow/flow.h>
#include <extension/external_async_awaitable.h>
#include <lease.hpp>
#include <array>
#include <memory>
#include <thread>
#include "tests/check.hpp"
namespace ff = flux_foundry;
using probe_error_t = ff::extension::external_async_error_t;
enum class mode { inline_ok, delayed, submit_fail, init_fail, collect_fail };
struct observations {
    int init=0, submit=0, callback=0, collect=0, destroy=0, free=0, resume=0;
    std::array<int, 12> order{};
    unsigned n=0;
    void event(int e) noexcept { order[n++]=e; }
};
struct response { observations* obs; int value; };
struct operation {
    mode how;
    observations* obs;
    void (*done)(void*) = nullptr;
    void* user = nullptr;
    response* result = nullptr;
    ~operation() { delete result; }
    void finish() noexcept {
        // Transport owns this stable state, not an awaitable pointer. Duplicate
        // replies are consumed here before dereferencing the FF callback user.
        auto callback=done;
        auto param=user;
        done=nullptr;
        user=nullptr;
        if (!callback) return;
        ++obs->callback;
        obs->event(3);
        callback(param); // Final access: may destroy context/awaitable inline.
    }
};
struct op {
    struct context_t { std::shared_ptr<operation>* state; };
    using result_t = response*;
    static int init_ctx(context_t* c, std::shared_ptr<operation>* p) noexcept {
        auto& o=**p;
        ++o.obs->init;
        o.obs->event(1);
        if (o.how==mode::init_fail) return -1;
        c->state=new(std::nothrow) std::shared_ptr<operation>(*p);
        return c->state ? 0 : -1;
    }
    static int submit(context_t* c, void(*cb)(void*), void* p) noexcept {
        // Keep independent state alive through inline completion.
        auto s=*c->state;
        ++s->obs->submit;
        s->obs->event(2);
        if (s->how==mode::submit_fail) return -1;
        if (s->how!=mode::collect_fail) {
            s->result=new(std::nothrow) response{s->obs, 42};
            if (!s->result) return -1;
        }
        s->done=cb;
        s->user=p;
        if (s->how!=mode::delayed) s->finish();
        return 0; // No context access after callback.
    }
    static result_t collect(context_t* c) noexcept {
        auto& s=**c->state;
        ++s.obs->collect;
        s.obs->event(4);
        return std::exchange(s.result, nullptr);
    }
    static void destroy_ctx(context_t* c) noexcept {
        auto& s=**c->state;
        ++s.obs->destroy;
        s.obs->event(6);
        CHECK(!s.done); // No early frame destruction supported in this probe.
        delete c->state;
    }
    static void free_result(response* p) noexcept { ++p->obs->free; p->obs->event(7); delete p; }
};
using result = ff::extension::external_async_awaitable<op>::async_result_type;
struct receiver {
    using value_type = result;
    observations* obs;
    bool expect_success;
    void emplace(result&& r) noexcept {
        ++obs->resume;
        obs->event(5);
        CHECK(r.has_value()==expect_success);
        if (r.has_value()) CHECK(r.value()->value==42);
    }
};
void run(mode how, bool threaded=false) {
    observations obs;
    auto state=std::make_shared<operation>();
    state->how=how; state->obs=&obs;
    auto bp=ff::make_blueprint<std::shared_ptr<operation>, probe_error_t>()
        | ff::await_external_async<op>()
        | ff::end([](result&& r) noexcept -> result { return std::move(r); });
    auto bp_ptr=ff::make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner=ff::make_runner(bp_ptr, receiver{&obs, how==mode::inline_ok || how==mode::delayed});
    runner(state);
    if (how==mode::delayed) {
        CHECK(obs.resume==0 && obs.destroy==0);
        if (threaded) { std::thread worker([state] { state->finish(); }); worker.join(); }
        else state->finish();
        state->finish(); // Late duplicate; FF context already destroyed.
    }
    CHECK(obs.init==1 && obs.resume==1);
    CHECK(obs.destroy==(how==mode::init_fail ? 0 : 1));
    CHECK(obs.callback==((how==mode::inline_ok || how==mode::delayed || how==mode::collect_fail) ? 1 : 0));
    CHECK(obs.collect==obs.callback);
    CHECK(obs.free==((how==mode::inline_ok || how==mode::delayed) ? 1 : 0));
    std::cout << "mode=" << static_cast<int>(how) << " threaded=" << threaded << " events=";
    for(unsigned i=0;i<obs.n;++i) std::cout << obs.order[i] << ',';
    std::cout << '\n';
}
struct referent { int value=8; };
struct lease_payload {
    std::shared_ptr<referent> backing;
    lease::access::exclusive_access<referent, lease::access::shared> access;
    lease_payload() : backing(std::make_shared<referent>()), access(lease::access::make_rw<lease::access::shared>(*backing)) {}
    lease_payload(lease_payload&&) noexcept = default;
    lease_payload& operator=(lease_payload&& other) noexcept {
        if(this!=&other) {this->~lease_payload();new(this) lease_payload(std::move(other));}
        return *this;
    }
};
void move_token_through_ff() {
    using r=ff::result_t<lease_payload,probe_error_t>;
    bool completed=false;
    auto bp=ff::make_blueprint<lease_payload,probe_error_t>() | ff::end([&](r&& in) noexcept -> r {
        CHECK(in.value().access->value==8); completed=true; return std::move(in);
    });
    auto bp_ptr=ff::make_lite_ptr<decltype(bp)>(std::move(bp));
    struct sink { using value_type=r; void emplace(r&& x) noexcept { CHECK(x.has_value()); } };
    auto runner=ff::make_runner(bp_ptr,sink{});
    runner(lease_payload{});
    CHECK(completed);
}
int main() {
    for (auto m : {mode::inline_ok,mode::delayed,mode::submit_fail,mode::init_fail,mode::collect_fail}) run(m);
    run(mode::delayed,true);
    move_token_through_ff();
}
