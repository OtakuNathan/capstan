#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/transport/io.hpp>
#include <dynabridge/extensions/flux_foundry/async.h>
namespace capstan {
namespace async_detail {
struct io_context {
    io_endpoint* endpoint;
    bytes request;
    std::shared_ptr<void> authority;
    io_completion completion;
};
struct io_operation {
    template<class Completion>
    dynabridge::async_submission operator()(io_context& c,Completion done) const noexcept {
        bool accepted=false;
        CAPSTAN_TRY {
            accepted=c.endpoint->submit_owned(std::move(c.request),[done](io_result value) noexcept {
                // A detachable endpoint independently retains the underlying transport
                // lifetime after its local Unknown delivery. The endpoint owns that layer.
                done.finish(dynabridge::async_result<io_result>(std::move(value)));
            },std::move(c.authority));
        } CAPSTAN_CATCH_ALL {accepted=false;}
        if(!accepted) return dynabridge::async_submission::reject(std::make_error_code(std::errc::resource_unavailable_try_again));
        return dynabridge::async_submission::accept();
    }
};
struct sink {
    using value_type=flux_foundry::result_t<io_result,dynabridge::async_error>;
    std::shared_ptr<io_context> context;
    void emplace(value_type&& value) noexcept {
        context->authority.reset(); // Also cover allocation failure before submit_owned took ownership.
        auto done=std::exchange(context->completion,nullptr);
        io_result output;
        if(value.has_value()) output=std::move(value.value());
        else output.state=outcome::not_submitted;
        if(done) {CAPSTAN_TRY {done(std::move(output));} CAPSTAN_CATCH_ALL {std::terminate();}}
    }
};
}
inline void start_io(io_endpoint& endpoint,bytes request,io_completion completion,std::shared_ptr<void> authority={}) {
    namespace ff=flux_foundry;
    std::shared_ptr<async_detail::io_context> context;
    CAPSTAN_TRY {
    context=std::make_shared<async_detail::io_context>();
    context->endpoint=&endpoint;context->request=std::move(request);context->authority=authority;context->completion=std::move(completion);
    auto prepared=dynabridge::make_invocation<io_result>(context,async_detail::io_operation{},std::move(authority));
#ifdef CAPSTAN_ERASED_INVOCATIONS
    auto invocation=dynabridge::erase_invocation(std::move(prepared));
#else
    auto invocation=std::move(prepared);
#endif
    using call_type=decltype(invocation);
    auto bp=ff::make_blueprint<call_type,dynabridge::async_error>()
        | dynabridge::ff::await_static_invocation<call_type>()
        | ff::end([](async_detail::sink::value_type&& r) noexcept -> async_detail::sink::value_type {return std::move(r);});
    auto owned=ff::make_lite_ptr<decltype(bp)>(std::move(bp));
#ifdef CAPSTAN_FAST_RUNNER
    if(!owned) {
#else
    auto controller=ff::make_lite_ptr<ff::flow_controller>();
    if(!owned || !controller) {
#endif
        // Release both prepared owners before an error continuation can reenter.
        context->authority.reset();
        {auto abandoned=std::move(invocation);}
        auto failed=std::exchange(context->completion,nullptr);failed({outcome::not_submitted,{}});return;
    }
#ifdef CAPSTAN_FAST_RUNNER
    auto runner=ff::make_fast_runner(owned,async_detail::sink{context});
#else
    ff::flow_runner<decltype(bp),async_detail::sink> runner(owned,controller,async_detail::sink{context});
#endif
    runner(std::move(invocation));
    } CAPSTAN_CATCH_ALL {
        authority.reset();
        auto done=context ? std::exchange(context->completion,nullptr) : std::move(completion);
        if(context) context->authority.reset();
        if(done) done({outcome::not_submitted,{}});
    }
}
}
