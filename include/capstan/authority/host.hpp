#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/protocol/types.hpp>
#include <algorithm>
#include <chrono>
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
    struct transfer_timing {id close_ns=0,drain_ns=0,install_ns=0;};
    static id monotonic_ns() noexcept {return static_cast<id>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
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
    struct transaction_record {transition target; transfer_request source; transfer_timing timing;};
    std::map<authority_tx_key, transaction_record> transactions_; // Bounded, never silently evicted.
    authority_tx_key pending_{};
    std::map<std::pair<id,id>, bool> requests_; // holder/request dedup, through epochs.
    std::size_t queue_limit_, history_limit_;
#ifdef CAPSTAN_BENCHMARK_BUILD
    benchmark_validation validation_=benchmark_validation::checked;
#endif
    status validate(const transition& t) const {
        if (t.key!=key_) return status::wrong_object;
        if (t.control_session!=control_ || t.origin!=authority_origin::directory || (t.issuer && t.issuer!=control_)) return status::wrong_control;
        if (!t.tx || t.expected_epoch==std::numeric_limits<id>::max() || t.next_epoch!=t.expected_epoch+1) return status::invalid;
        return validate_target(key_,t.next_epoch,t.destroy,t.grants);
    }
    void install_if_drained() noexcept {
        if (phase_!=phase::closing || !active_.empty()) return;
        auto& record=transactions_.at(pending_);
        auto& t=record.target;
        record.timing.drain_ns=monotonic_ns();
        // All storage was staged before Close. Completion cannot fail due to
        // allocation after removing the final pin.
        grants_.swap(pending_grants_);
        pending_grants_.clear();
        epoch_=t.next_epoch; // INSTALL linearization point, active is empty.
        phase_=t.destroy ? phase::dead : phase::open;
        record.timing.install_ns=monotonic_ns();
        pending_={};
    }
    status commit_locked(const transition& t,const transfer_request& source={}) {
        if(transactions_.size()>=history_limit_) return status::exhausted;
        auto staged=t.grants;
        transactions_.emplace(t.identity(),transaction_record{t,source,{}});
        pending_grants_.swap(staged);
        pending_=t.identity();
        phase_=phase::closing; // Shared CLOSE point for every authority origin.
        transactions_.at(pending_).timing.close_ns=monotonic_ns();
        install_if_drained();
        return pending_.sequence ? status::pending : status::ok;
    }
    transfer_receipt receipt_locked(const transaction_record& record) const {
        transfer_receipt result; result.reference=record.source.reference();
        if(pending_==record.target.identity()) {
            result.observation=transfer_observation::pending;result.code=status::pending;
            return result;
        }
        result.observation=transfer_observation::installed;
        result.installed_epoch=record.target.next_epoch;result.code=status::ok;
        if(phase_==phase::dead) result.usability=transfer_usability::dead;
        else if(epoch_!=record.target.next_epoch || grants_!=record.target.grants) result.usability=transfer_usability::superseded;
        else if(phase_==phase::closing) result.usability=transfer_usability::closed;
        else result.usability=transfer_usability::current;
        return result;
    }
    static bool business_holder(id holder) noexcept {return holder==1 || holder==2;}
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
        auto prior=transactions_.find(t.identity());
        if(prior!=transactions_.end()) {
            if(!(prior->second.target==t)) return status::conflict;
            install_if_drained();
            return pending_==t.identity() ? status::pending : status::ok; // Lost ACK retry.
        }
        if(phase_==phase::dead) return status::dead;
        if(phase_==phase::closing) return status::busy;
        if(t.expected_epoch!=epoch_) return status::stale;
        return commit_locked(t);
    }
    transfer_timing timing(authority_tx_key key) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto record=transactions_.find(key);return record==transactions_.end() ? transfer_timing{} : record->second.timing;
    }
    status transaction_status(authority_tx_key key) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!transactions_.count(key)) return status::not_seen;
        return pending_==key ? status::pending : status::ok;
    }
    status ack(id tx, acknowledgement& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        authority_tx_key key{authority_origin::directory,control_,tx};
        auto it=transactions_.find(key);
        if(it==transactions_.end()) return status::invalid;
        if(pending_==key) return status::pending;
        out.installed=it->second.target;
        return status::ok;
    }
    // mint runs only for a new, authenticated, currently valid request, before
    // any Close. It belongs to Host (OS randomness in the app; deterministic in
    // tests), never to a client-provided wire payload. It must not reenter Host.
    template<class Mint>
    status transfer_exclusive(const transfer_request& request,id caller,Mint mint,transfer_receipt& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        out={};out.reference=request.reference();
        auto reject=[&](status code) {out.observation=transfer_observation::rejected;out.code=code;return code;};
        if(request.source.key!=key_) return reject(status::wrong_object);
        if(request.transaction.origin!=authority_origin::holder_transfer || !request.transaction.sequence ||
           request.transaction.sequence==std::numeric_limits<id>::max()) return reject(status::invalid);
        if(!business_holder(caller) || caller!=request.source.holder || caller!=request.transaction.issuer)
            return reject(status::forbidden);
        auto prior=transactions_.find(request.transaction);
        if(prior!=transactions_.end()) {
            if(!(prior->second.source==request)) return reject(status::conflict);
            out=receipt_locked(prior->second);return out.code;
        }
        if(!business_holder(request.recipient) || request.recipient==caller) return reject(status::invalid);
        if(phase_==phase::dead) return reject(status::dead);
        if(phase_==phase::closing) return reject(status::busy);
        if(request.source.epoch!=epoch_) return reject(status::stale);
        if(request.source.mode!=access_mode::exclusive) return reject(status::forbidden);
        if(grants_.size()!=1 || !(grants_.front()==request.source)) return reject(status::bad_grant);
        if(epoch_==std::numeric_limits<id>::max() || transactions_.size()>=history_limit_) return reject(status::exhausted);
        grant successor;successor.key=key_;successor.epoch=epoch_+1;successor.holder=request.recipient;successor.mode=access_mode::exclusive;
        if(!mint(successor)) return reject(status::exhausted);
        if(successor.key!=key_ || successor.epoch!=epoch_+1 || successor.holder!=request.recipient ||
           successor.mode!=access_mode::exclusive || !successor.grant_id || successor.secret==token{} ||
           successor.secret==request.source.secret) return reject(status::invalid);
        transition target{key_,control_,request.transaction.sequence,epoch_,epoch_+1,false,{successor}};
        target.origin=authority_origin::holder_transfer;target.issuer=caller;
        auto code=commit_locked(target,request);
        if(code!=status::ok && code!=status::pending) return reject(code);
        out=receipt_locked(transactions_.at(request.transaction));return code;
    }
    transfer_receipt query_transfer(const transfer_ref& reference,id caller,bool control=false) const {
        std::lock_guard<std::mutex> lock(mutex_);
        transfer_receipt out;out.reference=reference;
        auto reject=[&](status code) {out.code=code;out.observation=transfer_observation::rejected;return out;};
        if(reference.key!=key_) return reject(status::wrong_object);
        if(!control && caller!=reference.from_holder && caller!=reference.to_holder) return reject(status::forbidden);
        if(reference.transaction.origin!=authority_origin::holder_transfer || reference.transaction.issuer!=reference.from_holder ||
           !reference.transaction.sequence) return reject(status::invalid);
        auto record=transactions_.find(reference.transaction);
        if(record==transactions_.end()) return out;
        if(!(record->second.source.reference()==reference)) return reject(status::conflict);
        return receipt_locked(record->second);
    }
    status claim_transfer(const transfer_ref& reference,id caller,grant& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if(reference.key!=key_) return status::wrong_object;
        if(!business_holder(caller) || caller!=reference.to_holder) return status::forbidden;
        auto record=transactions_.find(reference.transaction);
        if(record==transactions_.end()) return status::not_seen;
        if(record->first.origin!=authority_origin::holder_transfer || !(record->second.source.reference()==reference)) return status::conflict;
        auto receipt=receipt_locked(record->second);
        if(receipt.observation==transfer_observation::pending) return status::pending;
        if(receipt.usability==transfer_usability::dead) return status::dead;
        if(receipt.usability==transfer_usability::closed) return status::closed;
        if(receipt.usability!=transfer_usability::current) return status::superseded;
        out=record->second.target.grants.front();return status::ok;
    }
    status fetch_current(const object_key& key,id caller,grant& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if(key!=key_) return status::wrong_object;
        if(!business_holder(caller)) return status::forbidden;
        if(phase_==phase::dead) return status::dead;
        if(phase_!=phase::open) return status::closed;
        for(const auto& g:grants_) if(g.holder==caller) {out=g;return status::ok;}
        return status::bad_grant;
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
