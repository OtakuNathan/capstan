#pragma once
#include <capstan/protocol/types.hpp>
#include <limits>
#include <mutex>
namespace capstan {
// One control-session coordinator. epoch_ is an observation, not Host authority.
// A matching historical ACK is not proof that its grants are still current.
class directory_entry {
    mutable std::mutex mutex_;
    object_key key_;
    id control_, epoch_=0, next_tx_=1;
    bool pending_=false, dead_=false, ready_=false;
    transition current_;
    status begin_locked(std::vector<grant> target,bool destroy,transition& out) {
        if(pending_ || ready_) return status::busy;
        if(dead_) return status::dead;
        if(epoch_==std::numeric_limits<id>::max() || next_tx_==std::numeric_limits<id>::max()) return status::exhausted;
        transition t{key_,control_,next_tx_,epoch_,epoch_+1,destroy,std::move(target)};
        for(auto& g:t.grants) { g.key=key_; g.epoch=t.next_epoch; }
        const auto valid=validate_target(key_,t.next_epoch,destroy,t.grants);
        if(valid!=status::ok) return valid;
        out=t;
        current_=std::move(t);
        ++next_tx_;
        pending_=true;
        return status::ok;
    }
public:
    directory_entry(object_key key,id control) : key_(key),control_(control) {}
    status begin(std::vector<grant> target,bool destroy,transition& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        return begin_locked(std::move(target),destroy,out);
    }
    status observe(id epoch,bool dead=false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(epoch<epoch_) return status::stale;
        epoch_=epoch;dead_=dead_ || dead;
        if(ready_ && current_.next_epoch<epoch_) ready_=false;
        return status::ok;
    }
    status begin_at(id expected_epoch,std::vector<grant> target,bool destroy,transition& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(expected_epoch<epoch_) return status::stale;
        if(pending_ || ready_) return status::busy;
        epoch_=expected_epoch;
        return begin_locked(std::move(target),destroy,out);
    }
    status reject(const transition& target,status reason) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!pending_ || !(target==current_)) return status::conflict;
        if(reason==status::ok || reason==status::pending || reason==status::not_seen) return status::invalid;
        pending_=false;ready_=false;return reason;
    }
    status retry(transition& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!pending_) return status::invalid;
        out=current_;
        return status::ok;
    }
    status receive(const acknowledgement& ack) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!(ack.installed==current_)) return status::conflict;
        if(!pending_) return status::duplicate;
        pending_=false;
        if(current_.next_epoch<epoch_) {ready_=false;return status::stale;}
        epoch_=current_.next_epoch;
        dead_=current_.destroy;
        ready_=true; // Only now may the grants leave the directory.
        return status::ok;
    }
    status take_grants(std::vector<grant>& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!ready_) return status::pending;
        out=current_.grants;
        ready_=false;
        return status::ok;
    }
    id epoch() const { std::lock_guard<std::mutex> lock(mutex_); return epoch_; }
};
}
