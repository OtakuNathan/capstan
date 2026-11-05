#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/transport/io.hpp>
#include <memory>
#include <mutex>
namespace capstan {
// A local deadline may finish the caller as Unknown while the underlying
// transport keeps its own operation alive. No server cancellation is implied.
// The trigger is explicit; an owner-loop timer may call timeout_pending().
class detachable_endpoint final : public io_endpoint {
    struct registration {
        std::mutex mutex;
        io_completion callback;
        std::shared_ptr<void> lifetime;
        bool settled=false,submitting=true,queued=false;
        io_result queued_result;
        bool finish(io_result result) noexcept {
            io_completion done;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if(settled || queued) return false;
                if(submitting) {
                    queued_result=std::move(result);
                    queued=true;
                    return true;
                }
                settled=true;
                done=std::exchange(callback,nullptr);
            }
            if(done) { CAPSTAN_TRY { done(std::move(result)); } CAPSTAN_CATCH_ALL { std::terminate(); } }
            return true;
        }
        void network_finish(io_result result) noexcept {
            std::shared_ptr<void> release;
            {
                std::lock_guard<std::mutex> lock(mutex);
                release=std::move(lifetime);
            }
            // Real transport completion releases the detached authority before
            // normal FF completion, permitting reentrant next writes.
            release.reset();
            finish(std::move(result));
        }
        void submission_result(bool accepted) noexcept {
            io_completion done;
            io_result result;
            std::shared_ptr<void> release;
            {
                std::lock_guard<std::mutex> lock(mutex);
                submitting=false;
                if(!accepted) { settled=true;callback=nullptr;queued_result={};release=std::move(lifetime); }
                else if(queued) {
                    settled=true;done=std::exchange(callback,nullptr);result=std::move(queued_result);
                }
            }
            if(done) { CAPSTAN_TRY { done(std::move(result)); } CAPSTAN_CATCH_ALL { std::terminate(); } }
        }
        bool is_settled() { std::lock_guard<std::mutex> lock(mutex); return settled; }
    };
    io_endpoint& underlying_;
    std::mutex mutex_;
    std::shared_ptr<registration> current_;
public:
    explicit detachable_endpoint(io_endpoint& endpoint):underlying_(endpoint) {}
    bool submit(bytes request,io_completion done) noexcept override {
        return submit_owned(std::move(request),std::move(done),{});
    }
    bool submit_owned(bytes request,io_completion done,std::shared_ptr<void> lifetime) noexcept override {
        std::shared_ptr<registration> slot;
        CAPSTAN_TRY {
            slot=std::make_shared<registration>();
            slot->callback=std::move(done);
            slot->lifetime=std::move(lifetime);
            io_endpoint* transport=&underlying_;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if(current_ && !current_->is_settled()) return false;
                current_=slot;
            }
            // Only slot, never this or an FF context, is captured by late I/O.
            bool accepted=transport->submit(std::move(request),[slot](io_result result) noexcept { slot->network_finish(std::move(result)); });
            slot->submission_result(accepted);
            return accepted;
        } CAPSTAN_CATCH_ALL { if(slot) slot->submission_result(false); return false; }
    }
    bool timeout_pending() noexcept {
        std::shared_ptr<registration> slot;
        { std::lock_guard<std::mutex> lock(mutex_); slot=current_; }
        return slot && slot->finish({outcome::unknown,{}});
    }
};
}
