#pragma once
#include <capstan/async/io.hpp>
#include <chrono>
#include <capstan/migration/package.hpp>
#include <capstan/migration/wire.hpp>
#include <capstan/migration/decision.hpp>
#include <capstan/migration/clipboard.hpp>
namespace capstan { namespace migration {
// Session-owned Table operation. Every retry uses the same ticket, grant and
// checkpoint. Keep this record after Unknown; process restart recovery is not
// promised. Endpoints must own their transport lifetimes.
class move_operation : public std::enable_shared_from_this<move_operation> {
public:
    enum class step {reserve_source,reserve_target,freeze,export_snapshot,pull,fence,activate,retire,release,ready,abort_target,abort_source,abort_release,aborted};
private:
    ticket ticket_;
    grant successor_;
    std::shared_ptr<io_endpoint> source_,target_;
    std::string source_address_;
    unsigned short source_port_;
    std::size_t limit_;
    step step_=step::reserve_source;
    bool committed_=false,aborting_=false;
    std::uint64_t close_sent_ns_=0,activation_ack_ns_=0;
    static std::uint64_t now_ns() noexcept {return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    manifest manifest_;
    std::unique_ptr<decision> decision_;
    io_completion completion_;
    wire::value encoded_ticket() const {return wire::value::string(encode_ticket(ticket_));}
    void finish(outcome state,status code) noexcept {
        io_result result{outcome::unknown,{}};
        CAPSTAN_TRY {result={outcome::completed,wire::encode_reply(state,code)};} CAPSTAN_CATCH_ALL {}
        auto done=std::exchange(completion_,nullptr);
        if(done) {CAPSTAN_TRY {done(std::move(result));} CAPSTAN_CATCH_ALL {}}
    }
    void advance() noexcept {
        CAPSTAN_TRY {
            if(step_==step::ready || step_==step::aborted) {finish(outcome::completed,status::ok);return;}
            bytes packet;auto endpoint=source_;
            switch(step_) {
            case step::reserve_source: packet=wire::command(reserve_export_command,{encoded_ticket(),wire::u(limit_)});break;
            case step::reserve_target: endpoint=target_;packet=wire::command(reserve_command,{encoded_ticket(),wire::value::string(wire::encode_grant(successor_)),wire::u(limit_),wire::value::string(source_address_),wire::u(source_port_)});break;
            case step::freeze: if(!close_sent_ns_) close_sent_ns_=now_ns();packet=wire::command(freeze_command,{encoded_ticket()});break;
            case step::export_snapshot: packet=wire::command(export_command,{encoded_ticket()});break;
            case step::pull: endpoint=target_;packet=wire::command(pull_command,{encoded_ticket(),wire::value::string(source_address_),wire::u(source_port_)});break;
            case step::fence: packet=wire::command(fence_command,{encoded_ticket()});break;
            case step::activate: endpoint=target_;packet=wire::command(activate_command,{encoded_ticket()});break;
            case step::retire: packet=wire::command(retire_command,{encoded_ticket()});break;
            case step::release: case step::abort_release: packet=wire::command(export_release_command,{encoded_ticket()});break;
            case step::abort_target: endpoint=target_;packet=wire::command(import_abort_command,{encoded_ticket()});break;
            case step::abort_source: packet=wire::command(abort_command,{encoded_ticket(),wire::value::string(wire::encode_grant(successor_))});break;
            default: finish(outcome::unknown,status::invalid);return;
            }
            auto self=shared_from_this();
            // The existing FF invocation adapter supplies the static callable
            // path (or the build's explicit erased mode) for every control RPC.
            start_io(*endpoint,route(ticket_.source.key,std::move(packet)),[self](io_result io) noexcept {self->ack(std::move(io));},endpoint);
        } CAPSTAN_CATCH_ALL {finish(outcome::unknown,status::exhausted);}
    }
    void ack(io_result io) noexcept {
        CAPSTAN_TRY {
            wire::reply r;
            if(io.state!=outcome::completed || !wire::try_decode_reply(io.payload,r)) {finish(outcome::unknown,status::pending);return;}
            if(r.state!=outcome::completed || r.code!=status::ok) {finish(outcome::unknown,r.code);return;}
            switch(step_) {
            case step::reserve_source: step_=step::reserve_target;break;
            case step::reserve_target: step_=step::freeze;break;
            case step::freeze: {
                ticket echoed;
                if(r.values.size()!=2 || !wire::is_string(r.values[0]) || !decode_ticket(wire::string(r.values[0]),echoed) || !(echoed==ticket_) ||
                   !wire::is_number(r.values[1]) || wire::number(r.values[1])!=static_cast<unsigned>(authority_phase::frozen)) {finish(outcome::unknown,status::conflict);return;}
                step_=step::export_snapshot;break;
            }
            case step::export_snapshot:
                if(r.values.size()!=1 || !wire::is_string(r.values[0]) || !decode_manifest(wire::string(r.values[0]),manifest_) || !matches(manifest_,ticket_)) {finish(outcome::unknown,status::invalid);return;}
                decision_.reset(new decision(manifest_));decision_->frozen(manifest_,0);step_=step::pull;break;
            case step::pull:
                decision_->prepared(manifest_);
                if(decision_->commit(manifest_)!=status::ok) {finish(outcome::unknown,status::conflict);return;}
                committed_=true;step_=step::fence;break; // Irreversible Table decision BEFORE fencing.
            case step::fence: decision_->fenced(manifest_);step_=step::activate;break;
            case step::activate: activation_ack_ns_=now_ns();decision_->activated(manifest_);step_=step::retire;break;
            case step::retire: decision_->retired(manifest_);step_=step::release;break;
            case step::release: step_=step::ready;break;
            case step::abort_target: if(decision_) decision_->target_aborted(manifest_);step_=step::abort_source;break;
            case step::abort_source: if(decision_) decision_->source_restored(manifest_);step_=step::abort_release;break;
            case step::abort_release: step_=step::aborted;break;
            default: finish(outcome::unknown,status::invalid);return;
            }
            advance();
        } CAPSTAN_CATCH_ALL {finish(outcome::unknown,status::exhausted);}
    }
public:
    move_operation(ticket t,grant successor,std::shared_ptr<io_endpoint> source,std::shared_ptr<io_endpoint> target,
                   std::string source_address,unsigned short source_port,std::size_t limit)
        :ticket_(t),successor_(successor),source_(std::move(source)),target_(std::move(target)),source_address_(std::move(source_address)),source_port_(source_port),limit_(limit) {}
    const ticket& identity() const noexcept {return ticket_;}
    step checkpoint() const noexcept {return step_;}
    double observed_close_to_activation_seconds() const noexcept {return activation_ack_ns_ ? (activation_ack_ns_-close_sent_ns_)/1e9 : 0;}
    bool committed() const noexcept {return committed_;}
    bool ready() const noexcept {return decision_ && decision_->ready();}
    const grant& successor() const noexcept {return successor_;}
    const manifest& transferred() const noexcept {return manifest_;}
    // Submission starts a Cut; constructing this record is inert. A false
    // return means this observer was not attached and will receive no callback.
    bool resume(io_completion done) noexcept {
        if(completion_ || !done || !source_ || !target_) return false;
        completion_=std::move(done);advance();return true;
    }
    bool abort(io_completion done) noexcept {
        if(committed_ || completion_ || !done) return false;
        if(!aborting_) {
            if(decision_) decision_->abort(manifest_);
            aborting_=true;step_=step::abort_target;
        }
        return resume(std::move(done));
    }
};
}}
