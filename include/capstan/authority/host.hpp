#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/protocol/types.hpp>
#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
namespace capstan {
// One mutex is the decision domain for both authority changes and admission.
// Pins are explicit records, never RAII tied to a client handle. Only successful
// executor completion (or a dispatch rollback before start) removes a pin.
class host_authority {
public:
    enum class phase { open, closing, dead };
#ifdef CAPSTAN_BENCHMARK_BUILD
    // Available only to a separate, explicitly unsafe benchmark executable.
    // Never enable this definition on the production/demo Host target.
    enum class benchmark_validation { checked, omit_grant_checks };
#endif
    struct admission { status code; id pin=0; };
    struct snapshot { phase gate; id epoch; std::size_t active; id executing; std::vector<grant> grants; };
    struct operation { id pin; request call; bool executing=false; };
private:
    object_key key_;
    id control_;
    mutable std::mutex mutex_;
    phase phase_=phase::open;
    id epoch_=0, next_pin_=1;
    std::vector<grant> grants_;
    std::vector<grant> pending_grants_; // Fully allocated before Close commits.
    std::deque<operation> active_;
    std::map<id, transition> transactions_; // Bounded, never silently evicted.
    id pending_=0;
    std::map<std::pair<id,id>, bool> requests_; // holder/request dedup, through epochs.
    std::size_t queue_limit_, history_limit_;
#ifdef CAPSTAN_BENCHMARK_BUILD
    benchmark_validation validation_=benchmark_validation::checked;
#endif
    status validate(const transition& t) const {
        if (t.key!=key_) return status::wrong_object;
        if (t.control_session!=control_) return status::wrong_control;
        if (!t.tx || t.expected_epoch==std::numeric_limits<id>::max() || t.next_epoch!=t.expected_epoch+1) return status::invalid;
        return validate_target(key_,t.next_epoch,t.destroy,t.grants);
    }
    void install_if_drained() noexcept {
        if (phase_!=phase::closing || !active_.empty()) return;
        auto& t=transactions_.at(pending_);
        // All storage was staged before Close. Completion cannot fail due to
        // allocation after removing the final pin.
        grants_.swap(pending_grants_);
        pending_grants_.clear();
        epoch_=t.next_epoch; // INSTALL linearization point, active is empty.
        phase_=t.destroy ? phase::dead : phase::open;
        pending_=0;
    }
public:
    host_authority(object_key key, id control, std::size_t queue_limit=64, std::size_t history_limit=4096)
        : key_(key), control_(control), queue_limit_(queue_limit), history_limit_(history_limit) {}
#ifdef CAPSTAN_BENCHMARK_BUILD
    host_authority(object_key key, id control, benchmark_validation validation,
                   std::size_t queue_limit=64, std::size_t history_limit=4096)
        : host_authority(key,control,queue_limit,history_limit) { validation_=validation; }
#endif
    host_authority(const host_authority&)=delete;
    status set_authority(const transition& t) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto valid=validate(t);
        if(valid!=status::ok) return valid;
        auto prior=transactions_.find(t.tx);
        if(prior!=transactions_.end()) {
            if(!(prior->second==t)) return status::conflict;
            install_if_drained();
            return pending_==t.tx ? status::pending : status::ok; // Lost ACK retry.
        }
        if(phase_==phase::dead) return status::dead;
        if(phase_==phase::closing) return status::busy;
        if(t.expected_epoch!=epoch_) return status::stale;
        if(transactions_.size()>=history_limit_) return status::exhausted;
        auto staged_grants=t.grants;
        transactions_.emplace(t.tx,t); // All allocations before CLOSE commit.
        pending_grants_.swap(staged_grants);
        pending_=t.tx;
        phase_=phase::closing; // CLOSE linearization point.
        install_if_drained();
        return pending_ ? status::pending : status::ok;
    }
    status ack(id tx, acknowledgement& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=transactions_.find(tx);
        if(it==transactions_.end()) return status::invalid;
        if(pending_==tx) return status::pending;
        out.installed=it->second;
        return status::ok;
    }
    admission admit(const request& r, id bound_holder) {
        std::lock_guard<std::mutex> lock(mutex_);
#ifdef CAPSTAN_BENCHMARK_BUILD
        const bool validate_grant=validation_==benchmark_validation::checked;
#else
        constexpr bool validate_grant=true;
#endif
        if(validate_grant && r.capability.key!=key_) return {status::wrong_object};
        if(phase_==phase::dead) return {status::dead};
        if(phase_!=phase::open) return {status::closed};
        effect e;
        if(validate_grant) {
            if(r.capability.epoch!=epoch_) return {status::stale};
            if(r.capability.holder!=bound_holder) return {status::bad_grant};
            auto it=std::find(grants_.begin(),grants_.end(),r.capability);
            if(it==grants_.end()) return {status::bad_grant};
            if(!method_effect(r.operation,e)) return {status::unknown_method};
            if(e==effect::write && it->mode!=access_mode::exclusive) return {status::forbidden};
        } else {
            // The baseline retains dispatch allowlisting, all operation lifetime
            // machinery, deduplication and the same serialized request shape.
            if(!method_effect(r.operation,e)) return {status::unknown_method};
        }
        if(!r.request_id) return {status::invalid};
        auto req=std::make_pair(bound_holder,r.request_id);
        if(requests_.count(req)) return {status::duplicate};
        if(active_.size()>=queue_limit_) return {status::busy};
        if(requests_.size()>=history_limit_ || next_pin_==std::numeric_limits<id>::max()) return {status::exhausted};
        requests_.emplace(req,true);
        CAPSTAN_TRY { active_.push_back(operation{next_pin_,r,false}); }
        CAPSTAN_CATCH_ALL { requests_.erase(req); CAPSTAN_RETHROW; }
        // ADMIT linearization point: grant validation and pin insertion locked.
        return {status::ok,next_pin_++};
    }
    bool start_next(operation& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(active_.empty() || active_.front().executing) return false;
        active_.front().executing=true;
        out=active_.front();
        return true;
    }
    status complete(id pin) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(active_.empty() || active_.front().pin!=pin || !active_.front().executing) return status::invalid;
        active_.pop_front(); // COMPLETE point; caller has finished resource access.
        install_if_drained();
        return status::ok;
    }
    status dispatch_failed(id pin) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=std::find_if(active_.begin(),active_.end(),[pin](const operation& o){return o.pin==pin;});
        if(it==active_.end() || it->executing) return status::invalid;
        // Keep dedup record: this request was explicitly rejected, no retry.
        active_.erase(it);
        install_if_drained();
        return status::ok;
    }
    snapshot inspect() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {phase_,epoch_,active_.size(),active_.empty() || !active_.front().executing ? 0 : active_.front().pin,grants_};
    }
};
}
