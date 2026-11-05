#pragma once
#include <capstan/authority/host.hpp>
#include <capstan/migration/authority_wire.hpp>
#include <array>
namespace app {
class migration_replies {
    struct waiter {
        capstan::migration::ticket ticket;
        std::array<capstan::bytes,6> replies;
        capstan::io_completion done;
    };
    capstan::host_authority& authority_;
    std::array<waiter,64> waiters_{};
    std::size_t count_=0;
public:
    explicit migration_replies(capstan::host_authority& authority):authority_(authority) {}
    template<class Start> capstan::status submit(const capstan::migration::ticket& t,capstan::io_completion done,Start&& start) noexcept {
        using namespace capstan;
        CAPSTAN_TRY {
        if(count_==waiters_.size()) {reply(std::move(done),status::busy);return status::busy;}
        waiter staged;staged.ticket=t;
        for(unsigned phase=0;phase<staged.replies.size();++phase)
            staged.replies[phase]=wire::encode_reply(outcome::completed,status::ok,
                {wire::value::string(migration::encode_ticket(t)),wire::u(phase)});
        auto code=start();
        if(code!=status::ok && code!=status::pending) {reply(std::move(done),code);return code;}
        staged.done=std::move(done);waiters_[count_++]=std::move(staged);return code;
        } CAPSTAN_CATCH_ALL {
            if(done) {CAPSTAN_TRY {done({outcome::unknown,{}});} CAPSTAN_CATCH_ALL {}}
            return status::invalid;
        }
    }
    capstan::status freeze(const capstan::migration::ticket& t,capstan::io_completion done) noexcept {
        return submit(t,std::move(done),[this,&t] {return authority_.freeze_for_migration(t);});
    }
    capstan::status abort(const capstan::migration::ticket& t,const capstan::grant& replacement,capstan::io_completion done) noexcept {
        return submit(t,std::move(done),[this,&t,&replacement] {return authority_.abort_migration(t,replacement);});
    }
    void flush() {
        using namespace capstan;
        std::array<waiter,64> ready{};std::array<unsigned,64> phases{};
        std::size_t n=0,retained=0;
        for(std::size_t i=0;i<count_;++i) {
            migration::authority_phase state;
            if(authority_.migration_status(waiters_[i].ticket,state)!=status::ok) std::terminate();
            if(state==migration::authority_phase::draining || state==migration::authority_phase::aborting) {
                if(retained!=i) waiters_[retained]=std::move(waiters_[i]);
                ++retained;
            } else {phases[n]=static_cast<unsigned>(state);ready[n++]=std::move(waiters_[i]);}
        }
        for(std::size_t i=retained;i<count_;++i) waiters_[i]=waiter{};
        count_=retained;
        for(std::size_t i=0;i<n;++i) {
            auto done=std::exchange(ready[i].done,nullptr);
            CAPSTAN_TRY {done({outcome::completed,std::move(ready[i].replies[phases[i]])});} CAPSTAN_CATCH_ALL {}
        }
    }
};
}
