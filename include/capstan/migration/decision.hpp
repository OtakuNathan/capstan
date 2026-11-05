#pragma once
#include <capstan/migration/transfer.hpp>
#include <limits>
namespace capstan { namespace migration {
// Table decision record. Evidence enters only through authenticated Host replies
// bound to the complete immutable manifest; this class does not perform I/O.
class decision {
public:
    enum class terminal {undecided,commit,abort};
private:
    const manifest identity_;
    terminal decision_=terminal::undecided;
    bool frozen_=false,prepared_=false,fenced_=false,active_=false;
    bool target_aborted_=false,source_restored_=false,retired_=false;
    bool matches(const manifest& m) const noexcept {return identity_==m;}
public:
    explicit decision(manifest m) noexcept :identity_(m) {}
    terminal state() const noexcept {return decision_;}
    bool ready() const noexcept {return active_;}
    bool cleanup_pending() const noexcept {return active_ && !retired_;}
    bool aborted() const noexcept {return decision_==terminal::abort && target_aborted_ && source_restored_;}
    status frozen(const manifest& m,std::size_t pins) noexcept {
        if(!matches(m)) return status::conflict;
        if(decision_==terminal::abort) return status::closed;
        if(pins) return status::pending;
        frozen_=true;return status::ok;
    }
    status prepared(const manifest& m) noexcept {
        if(!matches(m)) return status::conflict;
        if(decision_==terminal::abort) return status::closed;
        if(!frozen_) return status::pending;
        prepared_=true;return status::ok;
    }
    status commit(const manifest& m) noexcept {
        if(!matches(m)) return status::conflict;
        if(decision_==terminal::abort) return status::conflict;
        if(!frozen_ || !prepared_) return status::pending;
        if(identity_.generation==std::numeric_limits<id>::max()) return status::exhausted;
        decision_=terminal::commit;return status::ok;
    }
    status fenced(const manifest& m) noexcept {
        if(!matches(m)) return status::conflict;
        if(decision_!=terminal::commit) return status::pending;
        fenced_=true;return status::ok;
    }
    status activated(const manifest& m) noexcept {
        if(!matches(m)) return status::conflict;
        if(decision_!=terminal::commit || !fenced_) return status::pending;
        active_=true;return status::ok;
    }
    status retired(const manifest& m) noexcept {
        if(!matches(m)) return status::conflict;
        if(!active_) return status::pending;
        retired_=true;return status::ok;
    }
    status abort(const manifest& m) noexcept {
        if(!matches(m) || decision_==terminal::commit) return status::conflict;
        decision_=terminal::abort;return status::ok;
    }
    status target_aborted(const manifest& m) noexcept {
        if(!matches(m)) return status::conflict;
        if(decision_!=terminal::abort) return status::conflict;
        target_aborted_=true;return status::ok;
    }
    status source_restored(const manifest& m) noexcept {
        if(!matches(m)) return status::conflict;
        if(decision_!=terminal::abort || !target_aborted_) return status::pending;
        source_restored_=true;return status::ok;
    }
};
}}
