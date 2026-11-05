#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/authority/host.hpp>
#include <capstan/protocol/wire.hpp>
#include <array>

namespace app {
// Owner-loop adapter. Each waiter belongs to one immutable transaction, not to
// the current global gate. Disconnect never releases business pins or authority.
class authority_replies {
    struct waiter {
        capstan::authority_tx_key transaction;
        capstan::bytes reply;
        std::array<capstan::bytes,5> transfer_replies;
        capstan::transfer_ref reference;
        capstan::io_completion done;
        bool transfer=false;
    };
    capstan::host_authority& authority_;
    std::array<waiter,64> waiting_{};
    std::size_t size_=0;
    static void finish(capstan::io_completion done,capstan::io_result result) noexcept {
        CAPSTAN_TRY { done(std::move(result)); } CAPSTAN_CATCH_ALL {}
    }
    static void reject(capstan::io_completion& done,capstan::status code) {
        auto response=capstan::wire::encode_reply(capstan::outcome::rejected,code);
        finish(std::move(done),{capstan::outcome::completed,std::move(response)});
    }
public:
    explicit authority_replies(capstan::host_authority& authority):authority_(authority) {}
    std::size_t size() const noexcept { return size_; }
    capstan::status set(const capstan::transition& target,capstan::io_completion done) noexcept {
        using namespace capstan;
        CAPSTAN_TRY {
            if(size_==waiting_.size()) {reject(done,status::busy);return status::busy;}
            waiter staged;staged.transaction=target.identity();
            staged.reply=wire::encode_reply(outcome::completed,status::ok,
                {wire::value::string(wire::binary(wire::encode_transition(target)))});
            const auto code=authority_.set_authority(target);
            if(code==status::pending || code==status::ok) {
                staged.done=std::move(done);waiting_[size_++]=std::move(staged);
            } else reject(done,code);
            return code;
        } CAPSTAN_CATCH_ALL {
            if(done) finish(std::move(done),{outcome::unknown,{}});
            return status::invalid;
        }
    }
    template<class Mint>
    capstan::status transfer(const capstan::transfer_request& request,capstan::id caller,Mint mint,capstan::io_completion done) noexcept {
        using namespace capstan;
        CAPSTAN_TRY {
            if(size_==waiting_.size()) {reject(done,status::busy);return status::busy;}
            waiter staged;staged.transaction=request.transaction;staged.reference=request.reference();staged.transfer=true;
            // All possible historical/current receipt encodings are allocated
            // before Close. No successor secret is present in any source reply.
            for(unsigned i=0;i<staged.transfer_replies.size();++i) {
                transfer_receipt receipt{staged.reference,transfer_observation::installed,
                    static_cast<transfer_usability>(i),request.source.epoch+1,status::ok};
                staged.transfer_replies[i]=wire::encode_reply(outcome::completed,status::ok,
                    {wire::value::string(wire::encode_transfer_receipt(receipt))});
            }
            transfer_receipt receipt;
            const auto code=authority_.transfer_exclusive(request,caller,mint,receipt);
            if(code==status::pending || code==status::ok) {
                staged.done=std::move(done);waiting_[size_++]=std::move(staged);
            } else reject(done,code);
            return code;
        } CAPSTAN_CATCH_ALL {
            if(done) finish(std::move(done),{outcome::unknown,{}});
            return status::invalid;
        }
    }
    void flush() {
        if(!size_) return;
        using namespace capstan;
        std::array<waiter,64> ready{};
        std::size_t count=0,remaining=0;
        for(std::size_t i=0;i<size_;++i) {
            if(authority_.transaction_status(waiting_[i].transaction)==status::pending) {
                if(remaining!=i) waiting_[remaining]=std::move(waiting_[i]);
                ++remaining;
            } else {
                auto& item=waiting_[i];
                if(item.transfer) {
                    const auto receipt=authority_.query_transfer(item.reference,item.reference.from_holder);
                    item.reply=std::move(item.transfer_replies[static_cast<unsigned>(receipt.usability)]);
                }
                ready[count++]=std::move(item);
            }
        }
        for(std::size_t i=remaining;i<size_;++i) waiting_[i]=waiter{};
        size_=remaining;
        // Detach and select all replies before callbacks. They may reenter,
        // start another transition or destroy this public adapter inline.
        for(std::size_t i=0;i<count;++i)
            finish(std::move(ready[i].done),{outcome::completed,std::move(ready[i].reply)});
    }
};
}
