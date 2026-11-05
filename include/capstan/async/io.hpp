#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/transport/io.hpp>
#include <capstan/async/errors.hpp>
#include <extension/external_async_awaitable.h>
#include <flow/flow.h>
#include <memory>
#include <mutex>
#include <new>
namespace capstan {
namespace async_detail {
struct state {
    io_endpoint* endpoint;
    bytes request;
    std::shared_ptr<void> authority;
    io_completion completion;
    std::mutex mutex;
    void (*callback)(void*)=nullptr;
    void* user=nullptr;
    io_result* result=nullptr;
    ~state() { delete result; }
    void finish(io_result value) noexcept {
        void (*done)(void*);
        void* parameter;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if(!callback) return; // Late duplicate never touches the awaitable.
            *result=std::move(value);
            done=callback; parameter=user;
            callback=nullptr; user=nullptr;
        }
        done(parameter); // Must be last: FF may collect/destroy inline.
    }
};
struct external_io {
    struct context_t { std::shared_ptr<state>* operation; };
    using result_t=io_result*;
    static int init_ctx(context_t* c,std::shared_ptr<state>* s) noexcept {
        c->operation=new(std::nothrow) std::shared_ptr<state>(*s);
        return c->operation ? 0 : -1;
    }
    static int submit(context_t* c,void(*done)(void*),void* user) noexcept {
        auto s=*c->operation;
        s->result=new(std::nothrow) io_result;
        if(!s->result) return -1;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->callback=done; s->user=user;
        }
        bool accepted=false;
        CAPSTAN_TRY {
            accepted=s->endpoint->submit_owned(std::move(s->request),[s](io_result value) noexcept { s->finish(std::move(value)); },s->authority);
        } CAPSTAN_CATCH_ALL { accepted=false; }
        if(!accepted) {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->callback=nullptr; s->user=nullptr;
            return -1;
        }
        // Endpoint owns its independent state; no context access after callback.
        return 0;
    }
    static result_t collect(context_t* c) noexcept {
        return std::exchange((*c->operation)->result,nullptr);
    }
    static void destroy_ctx(context_t* c) noexcept { delete c->operation; }
    static void free_result(result_t value) noexcept { delete value; }
};
using result=flux_foundry::extension::external_async_awaitable<external_io>::async_result_type;
struct sink {
    using value_type=result;
    std::shared_ptr<state> operation;
    void emplace(result&& value) noexcept {
        io_result output;
        if(value.has_value()) output=std::move(*value.value());
        else output.state=outcome::not_submitted;
        // Release the FF frame owner. A detachable endpoint retains another
        // owner until underlying response/disconnect, even after local timeout.
        // OutcomeUnknown never releases the independent Host operation pin.
        operation->authority.reset();
        // A moved-from std::function may still own its small callable (libc++).
        // Explicitly empty the slot even if transport retains a late callback.
        auto completion=std::exchange(operation->completion,nullptr);
        if(completion) {
            CAPSTAN_TRY { completion(std::move(output)); } CAPSTAN_CATCH_ALL { std::terminate(); }
        }
    }
};
}
// One-shot FF flow. FF continuations retain the lite_ptr blueprint/receiver
// after the stack runner has submitted. No cancellation handle is exposed, so
// dropping a caller handle cannot destroy a pending FF awaitable.
inline void start_io(io_endpoint& endpoint,bytes request,io_completion completion,std::shared_ptr<void> authority={}) {
    namespace ff=flux_foundry;
    auto s=std::make_shared<async_detail::state>();
    s->endpoint=&endpoint; s->request=std::move(request); s->authority=std::move(authority); s->completion=std::move(completion);
    auto bp=ff::make_blueprint<std::shared_ptr<async_detail::state>,ff::extension::external_async_error_t>()
        | ff::await_external_async<async_detail::external_io>()
        | ff::end([](async_detail::result&& r) noexcept -> async_detail::result { return std::move(r); });
    auto owned=ff::make_lite_ptr<decltype(bp)>(std::move(bp));
#ifdef CAPSTAN_FAST_RUNNER
    if(!owned) {
#else
    auto controller=ff::make_lite_ptr<ff::flow_controller>();
    if(!owned || !controller) {
#endif
        s->authority.reset();
        auto done=std::exchange(s->completion,nullptr);
        done({outcome::not_submitted,{}});
        return;
    }
#ifdef CAPSTAN_FAST_RUNNER
    // Same external I/O and operation lifetime; no runner cancellation API.
    auto runner=ff::make_fast_runner(owned,async_detail::sink{s});
#else
    ff::flow_runner<decltype(bp),async_detail::sink> runner(owned,controller,async_detail::sink{s});
#endif
    runner(s);
}
}
