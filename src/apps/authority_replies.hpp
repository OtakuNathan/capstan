#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/authority/host.hpp>
#include <capstan/protocol/wire.hpp>
#include <array>

namespace app {
// Owner-loop adapter, not part of the protocol state machine. A disconnected
// GIO session leaves only a weak completion here; it cannot remove a Host pin.
class authority_replies {
    struct waiter { capstan::bytes reply; capstan::io_completion done; };
    capstan::host_authority& authority_;
    std::array<waiter,64> waiting_{};
    std::size_t size_=0;

    static void finish(capstan::io_completion done,capstan::io_result result) noexcept {
        // A response transport failure is lost ACK, never authority rollback.
        // Do not invoke a throwing completion a second time.
        CAPSTAN_TRY { done(std::move(result)); } CAPSTAN_CATCH_ALL {}
    }
public:
    explicit authority_replies(capstan::host_authority& authority):authority_(authority) {}
    std::size_t size() const noexcept { return size_; }
    capstan::status set(const capstan::transition& target,capstan::io_completion done) noexcept {
        using namespace capstan;
        CAPSTAN_TRY {
            if(size_==waiting_.size()) {
                auto rejected=wire::encode_reply(outcome::rejected,status::busy);
                finish(std::move(done),{outcome::completed,std::move(rejected)});
                return status::busy;
            }
            // Reserve bounded storage and encode the exact future ACK BEFORE
            // Close. After set_authority commits, retaining it cannot allocate.
            auto response=wire::encode_reply(outcome::completed,status::ok,
                {wire::value::string(wire::binary(wire::encode_transition(target)))});
            const auto code=authority_.set_authority(target);
            if(code==status::pending || code==status::ok) {
                waiting_[size_++]={std::move(response),std::move(done)};
            } else {
                auto rejected=wire::encode_reply(outcome::rejected,code);
                finish(std::move(done),{outcome::completed,std::move(rejected)});
            }
            return code;
        } CAPSTAN_CATCH_ALL {
            finish(std::move(done),{outcome::unknown,{}});
            return status::invalid;
        }
    }
    void flush() {
        using namespace capstan;
        if(!size_ || authority_.inspect().gate==host_authority::phase::closing) return;
        // The single authority can leave Closing only after all old pins drain.
        // All retained transactions are now installed (including old retries).
        // Detach the batch before callbacks: they may complete/reenter inline.
        std::array<waiter,64> ready{};
        ready.swap(waiting_);
        const auto count=size_; size_=0;
        for(std::size_t i=0;i<count;++i)
            finish(std::move(ready[i].done),{outcome::completed,std::move(ready[i].reply)});
    }
};
}
