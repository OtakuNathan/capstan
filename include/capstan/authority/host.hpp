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
#include <memory>
#include <set>
#include <capstan/migration/authority.hpp>
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
    std::set<id> captures_; // Independent backend readers; never the business FIFO.
    struct transaction_record {transition target; transfer_request source; transfer_timing timing;};
    std::map<authority_tx_key, transaction_record> transactions_; // Bounded, never silently evicted.
    authority_tx_key pending_{};
    std::map<std::pair<id,id>, bool> requests_; // holder/request dedup, through epochs.
    std::size_t queue_limit_, history_limit_;
    id placement_generation_=1,outgoing_=0;
    std::map<id,migration::authority_record> migrations_;
    struct incoming_state {
        migration::ticket ticket;
        bool prepared=false;
        id previous_outgoing=0;
        migration::authority_image image;
        migration::authority_image original;
        std::map<authority_tx_key,transaction_record> transactions;
        std::vector<grant> grants;
    };
    std::unique_ptr<incoming_state> incoming_;
    status validate_migration(const migration::ticket& t) const noexcept {
        if(t.control!=control_) return status::wrong_control;
        if(t.source.key!=key_) return status::wrong_object;
        if(!t.sequence || !t.source_host || !t.target_host || t.source_host==t.target_host ||
           !t.source_instance || !t.target_instance || !t.generation || t.generation==std::numeric_limits<id>::max() ||
           t.source.epoch==std::numeric_limits<id>::max() || t.source.mode!=access_mode::exclusive) return status::invalid;
        return status::ok;
    }
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
        if (phase_!=phase::closing || !active_.empty() || !captures_.empty()) return;
        if(incoming_) return;
        if(outgoing_) {
            auto& migration=migrations_.at(outgoing_);
            if(migration.state==migration::authority_phase::draining) migration.state=migration::authority_phase::frozen;
            if(migration.state==migration::authority_phase::aborting) {
                grants_.swap(pending_grants_);pending_grants_.clear();
                ++epoch_;migration.state=migration::authority_phase::aborted;
                outgoing_=0;phase_=phase::open;
            }
            return;
        }
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
        if(captures_.erase(pin)) {install_if_drained();return status::ok;}
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
    // The runtime must authenticate the established controller and bind both
    // Host instances before invoking migration control methods. All transitions
    // below share the real admission/Drain mutex; no caller timeout removes pins.
    status freeze_for_migration(const migration::ticket& t) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto code=validate_migration(t);if(code!=status::ok) return code;
        auto old=migrations_.find(t.sequence);
        if(old!=migrations_.end()) {
            if(!(old->second.request==t)) return status::conflict;
            auto state=old->second.state;
            if(state==migration::authority_phase::aborted || state==migration::authority_phase::aborting) return status::closed;
            return state==migration::authority_phase::draining ? status::pending : status::ok;
        }
        if(phase_!=phase::open) return phase_==phase::dead ? status::dead : status::busy;
        if(t.generation!=placement_generation_ || t.source.epoch!=epoch_) return status::stale;
        if(grants_.size()!=1 || !(grants_.front()==t.source)) return status::bad_grant;
        if(migrations_.size()>=history_limit_) return status::exhausted;
        migrations_.emplace(t.sequence,migration::authority_record{t,migration::authority_phase::draining,{}});
        outgoing_=t.sequence;phase_=phase::closing;install_if_drained();
        return active_.empty() && captures_.empty() ? status::ok : status::pending;
    }
    status migration_status(const migration::ticket& t,migration::authority_phase& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto code=validate_migration(t);if(code!=status::ok) return code;
        auto found=migrations_.find(t.sequence);if(found==migrations_.end()) return status::not_seen;
        if(!(found->second.request==t)) return status::conflict;
        out=found->second.state;return status::ok;
    }
    // Capture is a separate backend pin. Opt-in early capture is legal only
    // after Close with an entirely read-only admitted set, including queued
    // operations. It cannot allow Freeze/Abort to complete ahead of its reader.
    admission begin_migration_capture(const migration::ticket& t,bool overlap_readers=false) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found=migrations_.find(t.sequence);
        if(found==migrations_.end()) return {status::not_seen};
        if(!(found->second.request==t)) return {status::conflict};
        const auto state=found->second.state;
        if(outgoing_!=t.sequence || (state!=migration::authority_phase::frozen && !(overlap_readers && state==migration::authority_phase::draining))) return {status::closed};
        if(!captures_.empty()) return {status::busy};
        if(!active_.empty()) {
            if(!overlap_readers) return {status::busy};
            for(const auto& op:active_) {effect e;if(!method_effect(op.call.operation,e) || e!=effect::read) return {status::busy};}
        }
        if(next_pin_==std::numeric_limits<id>::max()) return {status::exhausted};
        captures_.insert(next_pin_);return {status::ok,next_pin_++};
    }
    status export_authority(const migration::ticket& t,migration::authority_image& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found=migrations_.find(t.sequence);
        if(found==migrations_.end()) return status::not_seen;
        if(!(found->second.request==t)) return status::conflict;
        if(found->second.state==migration::authority_phase::draining) return status::pending;
        if(outgoing_!=t.sequence || found->second.state!=migration::authority_phase::frozen) return status::closed;
        if(!active_.empty() || !captures_.empty()) return status::pending;
        migration::authority_image staged;staged.key=key_;staged.control=control_;staged.epoch=epoch_;staged.generation=placement_generation_;
        staged.requests=requests_;staged.migrations=migrations_;
        for(const auto& item:transactions_) staged.transactions.emplace(item.first,migration::control_record{item.second.target,item.second.source});
        out=std::move(staged);return status::ok;
    }
    status fence_migration(const migration::ticket& t) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found=migrations_.find(t.sequence);if(found==migrations_.end()) return status::not_seen;
        if(!(found->second.request==t)) return status::conflict;
        auto& state=found->second.state;
        if(state==migration::authority_phase::fenced) return status::ok;
        if(outgoing_!=t.sequence || !active_.empty() || !captures_.empty() || state!=migration::authority_phase::frozen) return status::conflict;
        state=migration::authority_phase::fenced;return status::ok;
    }
    // Invoke only after Table has decided Abort and the destination has ACKed
    // its abort tombstone. A fresh grant fences every old local/raw handle.
    status abort_migration(const migration::ticket& t,const grant& replacement) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(incoming_) return status::busy;
        auto code=validate_migration(t);if(code!=status::ok) return code;
        auto found=migrations_.find(t.sequence);
        if(found!=migrations_.end()) {
            if(!(found->second.request==t)) return status::conflict;
            if(found->second.state==migration::authority_phase::aborted) return found->second.result_grants==std::vector<grant>{replacement} ? status::ok : status::conflict;
            if(found->second.state==migration::authority_phase::fenced || found->second.state==migration::authority_phase::installed) return status::conflict;
            if(found->second.state==migration::authority_phase::aborting) return found->second.result_grants==std::vector<grant>{replacement} ? status::pending : status::conflict;
        } else {
            if(migrations_.size()>=history_limit_) return status::exhausted;
            // Record unseen aborts even if another authority decision won. This
            // prevents the delayed original Freeze from becoming a new decision.
            if(phase_!=phase::open || placement_generation_!=t.generation || grants_.size()!=1 || !(grants_.front()==t.source)) {
                migrations_.emplace(t.sequence,migration::authority_record{t,migration::authority_phase::aborted,{replacement}});
                return status::ok;
            }
        }
        if(replacement.key!=key_ || replacement.epoch!=epoch_+1 || replacement.holder!=t.source.holder ||
           replacement.mode!=access_mode::exclusive || !replacement.grant_id || replacement.secret==token{} || replacement.secret==t.source.secret)
            return status::invalid;
        std::vector<grant> rollback{replacement};
        std::vector<grant> install{replacement};
        if(found==migrations_.end()) found=migrations_.emplace(t.sequence,migration::authority_record{t,migration::authority_phase::draining,{}}).first;
        found->second.result_grants.swap(rollback);pending_grants_.swap(install);found->second.state=migration::authority_phase::aborting;
        outgoing_=t.sequence;phase_=phase::closing;install_if_drained();
        return active_.empty() && captures_.empty() ? status::ok : status::pending;
    }
    // Reserve the destination gate BEFORE receiving bytes. This shares the
    // authority mutex with ordinary grants and survives a disconnected observer.
    status reserve_import(const migration::ticket& t) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto code=validate_migration(t);if(code!=status::ok) return code;
        if(incoming_) return incoming_->ticket==t ? status::ok : status::busy;
        auto prior=migrations_.find(t.sequence);
        if(prior!=migrations_.end()) return prior->second.request==t && prior->second.state==migration::authority_phase::installed ? status::ok : status::conflict;
        if(!active_.empty() || !captures_.empty() || phase_==phase::dead || (phase_==phase::closing && !outgoing_) ||
           (epoch_ && (!outgoing_ || migrations_.at(outgoing_).state!=migration::authority_phase::fenced))) return status::busy;
        if(epoch_ && t.generation<=placement_generation_) return status::stale;
        if(migrations_.size()>=history_limit_) return status::exhausted;
        auto staged=std::unique_ptr<incoming_state>(new incoming_state);
        staged->ticket=t;staged->previous_outgoing=outgoing_;
        incoming_=std::move(staged);outgoing_=0;phase_=phase::closing;return status::ok;
    }
    // The image has already been authenticated and decoded. Stage allocates all
    // history/grant storage; activation is an allocation-free ownership swap.
    status stage_import(const migration::ticket& t,const migration::authority_image& image,const grant& successor) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto code=validate_migration(t);if(code!=status::ok) return code;
        if(incoming_ && !(incoming_->ticket==t)) return status::conflict;
        if(incoming_ && incoming_->prepared) return incoming_->original==image && incoming_->grants.front()==successor ? status::ok : status::conflict;
        auto prior=migrations_.find(t.sequence);
        if(prior!=migrations_.end()) return prior->second.request==t && prior->second.state==migration::authority_phase::installed && prior->second.result_grants==std::vector<grant>{successor} ? status::ok : status::conflict;
        if(!incoming_ && (!active_.empty() || !captures_.empty() || phase_==phase::dead || (phase_==phase::closing && !outgoing_) || (epoch_ && (!outgoing_ || migrations_.at(outgoing_).state!=migration::authority_phase::fenced)))) return status::busy;
        if(image.key!=key_ || image.control!=control_ || image.epoch!=t.source.epoch || image.generation!=t.generation ||
           (epoch_ && t.generation<=placement_generation_)) return status::stale;
        if(image.requests.size()>history_limit_ || image.transactions.size()>history_limit_ || image.migrations.size()>history_limit_) return status::exhausted;
        auto origin=image.migrations.find(t.sequence);
        if(origin==image.migrations.end() || !(origin->second.request==t) || origin->second.state!=migration::authority_phase::frozen) return status::invalid;
        if(successor.key!=key_ || successor.epoch!=image.epoch+1 || successor.holder!=t.source.holder || successor.mode!=access_mode::exclusive ||
           !successor.grant_id || successor.secret==token{} || successor.secret==t.source.secret) return status::invalid;
        if(migrations_.size()>=history_limit_) return status::exhausted;
        auto staged=std::unique_ptr<incoming_state>(new incoming_state);
        staged->previous_outgoing=incoming_ ? incoming_->previous_outgoing : outgoing_;
        staged->prepared=true;staged->ticket=t;staged->original=image;staged->image=image;staged->grants={successor};
        staged->image.migrations.at(t.sequence).result_grants={successor};
        for(const auto& req:requests_) if(!image.requests.count(req.first)) return status::conflict;
        for(const auto& tx:transactions_) {
            auto newer=image.transactions.find(tx.first);
            if(newer==image.transactions.end() || !(newer->second.target==tx.second.target) || !(newer->second.source==tx.second.source)) return status::conflict;
        }
        for(const auto& old:migrations_) {
            auto newer=staged->image.migrations.find(old.first);
            if(newer!=staged->image.migrations.end() && !(newer->second.request==old.second.request)) return status::conflict;
            if(newer==staged->image.migrations.end()) staged->image.migrations.emplace(old);
        }
        if(staged->image.migrations.size()>history_limit_) return status::exhausted;
        for(const auto& entry:image.transactions) {
            if(entry.first!=entry.second.target.identity() || entry.second.target.key!=key_ || entry.second.target.control_session!=control_ ||
               entry.second.target.next_epoch>image.epoch) return status::invalid;
            staged->transactions.emplace(entry.first,transaction_record{entry.second.target,entry.second.source,{}});
        }
        incoming_=std::move(staged);outgoing_=0;phase_=phase::closing;return status::ok;
    }
    status abort_import(const migration::ticket& t) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto code=validate_migration(t);if(code!=status::ok) return code;
        if(incoming_ && !(incoming_->ticket==t)) return status::busy;
        auto prior=migrations_.find(t.sequence);
        if(prior!=migrations_.end()) {
            if(!(prior->second.request==t)) return status::conflict;
            return prior->second.state==migration::authority_phase::aborted ? status::ok : status::conflict;
        }
        if(migrations_.size()>=history_limit_) return status::exhausted;
        migrations_.emplace(t.sequence,migration::authority_record{t,migration::authority_phase::aborted,{}});
        if(incoming_) {
            outgoing_=incoming_->previous_outgoing;incoming_.reset();
            phase_=epoch_ ? phase::closing : phase::open;
        }
        return status::ok;
    }
    // Only the established Table may activate, AFTER its matching source-fence ACK.
    status activate_import(const migration::ticket& t) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!incoming_) {
            auto prior=migrations_.find(t.sequence);
            return prior!=migrations_.end() && prior->second.request==t && prior->second.state==migration::authority_phase::installed ? status::ok : status::conflict;
        }
        if(!(incoming_->ticket==t)) return status::conflict;
        if(!incoming_->prepared) return status::pending;
        requests_.swap(incoming_->image.requests);transactions_.swap(incoming_->transactions);migrations_.swap(incoming_->image.migrations);
        grants_.swap(incoming_->grants);epoch_=t.source.epoch+1;placement_generation_=t.generation+1;
        migrations_.at(t.sequence).state=migration::authority_phase::installed;
        incoming_.reset();phase_=phase::open;return status::ok;
    }
    id placement_generation() const noexcept {std::lock_guard<std::mutex> lock(mutex_);return placement_generation_;}
    struct occupancy {std::size_t readers=0,writers=0,captures=0;};
    occupancy inspect_occupancy() const {
        std::lock_guard<std::mutex> lock(mutex_);
        occupancy result;result.captures=captures_.size();
        for(const auto& op:active_) {effect e;if(method_effect(op.call.operation,e) && e==effect::read) ++result.readers;else ++result.writers;}
        return result;
    }
    snapshot inspect() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {phase_,epoch_,active_.size()+captures_.size(),active_.empty() || !active_.front().executing ? 0 : active_.front().pin,grants_};
    }
};
}
