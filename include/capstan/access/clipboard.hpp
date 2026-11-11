#pragma once
#include <capstan/access/capability.hpp>
#include <capstan/migration/clipboard.hpp>
#include <capstan/migration/coordinator.hpp>
namespace capstan {
namespace clipboard_detail {
struct command_context {std::shared_ptr<io_endpoint> endpoint;bytes packet;std::shared_ptr<void> borrow;};
template<class R,class Decode>
auto command(std::shared_ptr<io_endpoint> endpoint,bytes packet,Decode decode,std::shared_ptr<void> borrow={}) {
    auto context=std::make_shared<command_context>();context->endpoint=std::move(endpoint);context->packet=std::move(packet);context->borrow=borrow;
    auto prepared=portal::make_invocation<R>(context,[decode=std::move(decode)](command_context& ctx,auto completion) noexcept {
        CAPSTAN_TRY {
            auto borrow=std::move(ctx.borrow);
            if(!ctx.endpoint) return portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::invalid));
            bool accepted=ctx.endpoint->submit_owned(std::move(ctx.packet),[decode,completion](io_result io) noexcept {
                CAPSTAN_TRY {
                    wire::reply reply;
                    if(io.state!=outcome::completed || !wire::try_decode_reply(io.payload,reply)) {
                        completion.finish(portal::async_result<R>(make_rpc_error(io.state==outcome::not_submitted ? io.state : outcome::unknown,status::invalid)));return;
                    }
                    if(reply.state!=outcome::completed || reply.code!=status::ok) {completion.finish(portal::async_result<R>(make_rpc_error(reply.state,reply.code)));return;}
                    R value;
                    if(!decode(reply,value)) completion.finish(portal::async_result<R>(make_rpc_error(outcome::unknown,status::invalid)));
                    else completion.finish(portal::async_result<R>(std::move(value)));
                } CAPSTAN_CATCH_ALL {completion.finish(portal::async_result<R>(make_rpc_error(outcome::unknown,status::exhausted)));}
            },std::move(borrow));
            return accepted ? portal::async_submission::accept() : portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::rejected));
        } CAPSTAN_CATCH_ALL {return portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::exhausted));}
    },std::move(borrow));
#ifdef CAPSTAN_ERASED_INVOCATIONS
    return portal::erase_invocation(std::move(prepared));
#else
    return prepared;
#endif
}
}
// Fresh owning invocation per BP run. Constructing it captures a local read
// borrow; actual native capture is serialized by the source Host at submission.
template<class Readable>
auto copy(std::shared_ptr<io_endpoint> endpoint,const Readable& access,migration::copy_ref& reference) {
    auto read=access.summary();auto source=read.call.header.capability;auto sequence=read.request_id;
    reference={source.key,source.holder,sequence};
    auto packet=migration::route(source.key,wire::command(migration::copy_command,{wire::value::string(wire::encode_grant(source)),wire::u(sequence)}));
    return clipboard_detail::command<migration::snapshot_ref>(std::move(endpoint),std::move(packet),[source,sequence](const wire::reply& reply,migration::snapshot_ref& ref) noexcept {
        CAPSTAN_TRY {return reply.values.size()==1 && wire::is_string(reply.values[0]) && migration::decode_snapshot_ref(wire::string(reply.values[0]),ref) && ref.source==source.key && ref.holder==source.holder && ref.sequence==sequence;}
        CAPSTAN_CATCH_ALL {return false;}
    },std::move(read.authority));
}
template<class Readable>
auto copy(std::shared_ptr<io_endpoint> endpoint,const Readable& access) {
    migration::copy_ref ignored;return copy(std::move(endpoint),access,ignored);
}
inline auto query_copy(std::shared_ptr<io_endpoint> endpoint,migration::copy_ref reference) {
    auto packet=migration::route(reference.source,wire::command(migration::copy_query_command,{wire::u(reference.sequence)}));
    return clipboard_detail::command<migration::snapshot_ref>(std::move(endpoint),std::move(packet),[reference](const wire::reply& reply,migration::snapshot_ref& ref) noexcept {
        CAPSTAN_TRY {return reply.values.size()==1 && wire::is_string(reply.values[0]) && migration::decode_snapshot_ref(wire::string(reply.values[0]),ref) && ref.source==reference.source && ref.holder==reference.holder && ref.sequence==reference.sequence;}
        CAPSTAN_CATCH_ALL {return false;}
    });
}
inline auto release_snapshot(std::shared_ptr<io_endpoint> endpoint,migration::snapshot_ref reference) {
    auto packet=migration::route(reference.source,wire::command(migration::copy_release_command,{wire::value::string(migration::encode_snapshot_ref(reference))}));
    return clipboard_detail::command<bool>(std::move(endpoint),std::move(packet),[](const wire::reply& reply,bool& released) noexcept {released=reply.values.empty();return released;});
}
struct clone_receipt {enum class phase {receiving,installed,aborted};phase state=phase::receiving;grant original;};
inline auto query_clone(std::shared_ptr<io_endpoint> endpoint,id sequence) {
    return clipboard_detail::command<clone_receipt>(std::move(endpoint),wire::command(migration::clone_query_command,{wire::u(sequence)}),[](const wire::reply& reply,clone_receipt& out) noexcept {
        CAPSTAN_TRY {
            if(reply.values.empty() || !wire::is_number(reply.values[0]) || wire::number(reply.values[0])>2) return false;
            out.state=static_cast<clone_receipt::phase>(wire::number(reply.values[0]));
            if(out.state==clone_receipt::phase::installed) return reply.values.size()==2 && wire::is_string(reply.values[1]) && wire::try_decode_grant(wire::string(reply.values[1]),out.original);
            return reply.values.size()==1;
        } CAPSTAN_CATCH_ALL {return false;}
    });
}
inline auto abort_clone(std::shared_ptr<io_endpoint> endpoint,id sequence) {
    return clipboard_detail::command<bool>(std::move(endpoint),wire::command(migration::clone_abort_command,{wire::u(sequence)}),[](const wire::reply& reply,bool& aborted) noexcept {
        aborted=reply.values.size()==1 && wire::is_number(reply.values[0]) && wire::number(reply.values[0])==2;return aborted;
    });
}
inline auto paste(std::shared_ptr<io_endpoint> endpoint,migration::snapshot_ref snapshot,object_key new_key,id sequence,std::string source_address,unsigned short source_port) {
    auto packet=wire::command(migration::clone_command,{wire::u(sequence),wire::value::string(migration::encode_snapshot_ref(snapshot)),wire::u(new_key.cluster),wire::u(new_key.incarnation),wire::u(new_key.object),wire::value::string(std::move(source_address)),wire::u(source_port)});
    return clipboard_detail::command<grant>(std::move(endpoint),std::move(packet),[new_key,snapshot](const wire::reply& reply,grant& result) noexcept {
        CAPSTAN_TRY {return reply.values.size()==2 && wire::is_number(reply.values[0]) && wire::number(reply.values[0])==1 && wire::is_string(reply.values[1]) &&
            wire::try_decode_grant(wire::string(reply.values[1]),result) && result.key==new_key && result.holder==snapshot.holder && result.mode==access_mode::exclusive && result.epoch==1;}
        CAPSTAN_CATCH_ALL {return false;}
    });
}
// Cut's trusted Table operation contains the private grant; expose only this
// non-authorizing identity to clipboard UIs. The record is session-scoped.
struct cut_ref {object_key object;id holder=0,control=0,sequence=0,source_host=0,source_instance=0,generation=0;};
class cut_plan {
    struct state {std::shared_ptr<access_detail::exclusive_state> owner;std::shared_ptr<migration::move_operation> operation;std::atomic<bool> sealed{false};};
    std::shared_ptr<state> state_;
    explicit cut_plan(std::shared_ptr<state> state):state_(std::move(state)) {}
public:
    static std::unique_ptr<cut_plan> prepare(const exclusive_capability& source,std::shared_ptr<migration::move_operation> operation,status& code) {
        code=status::invalid;
        if(!source.owner_ || !operation || !(source.owner_->identity==operation->identity().source)) return {};
        {std::lock_guard<std::mutex> lock(source.owner_->mutex);if(source.owner_->closed) {code=status::closed;return {};}}
        auto shared=std::make_shared<state>();shared->owner=source.owner_;shared->operation=std::move(operation);
        auto result=std::unique_ptr<cut_plan>(new cut_plan(std::move(shared)));code=status::ok;return result;
    }
    cut_ref reference() const noexcept {const auto& t=state_->operation->identity();return {t.source.key,t.source.holder,t.control,t.sequence,t.source_host,t.source_instance,t.generation};}
    bool sealed() const noexcept {return state_->sealed.load();}
    // Local sealing occurs when FF submits this invocation, never at prepare.
    auto paste() const {
        return portal::make_invocation<io_result>(state_,[](state& s,auto done) noexcept {
            CAPSTAN_TRY {
            if(s.sealed || !access_detail::seal(s.owner)) return portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::busy));
            s.sealed=true;
            bool accepted=s.operation->resume([done](io_result result) noexcept {done.finish(portal::async_result<io_result>(std::move(result)));});
            return accepted ? portal::async_submission::accept() : portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::busy));
            } CAPSTAN_CATCH_ALL {return portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::exhausted));}
        });
    }
    auto reconcile() const {
        return portal::make_invocation<io_result>(state_,[](state& s,auto done) noexcept {
            CAPSTAN_TRY {
            if(!s.sealed) return portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::invalid));
            bool accepted=s.operation->resume([done](io_result result) noexcept {done.finish(portal::async_result<io_result>(std::move(result)));});
            return accepted ? portal::async_submission::accept() : portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::busy));
            } CAPSTAN_CATCH_ALL {return portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::exhausted));}
        });
    }
    // Abort never reopens the sealed facade. Materialize the fresh returned
    // source grant only after the coordinator reports its completed Abort.
    auto abort() const {
        return portal::make_invocation<io_result>(state_,[](state& s,auto done) noexcept {
            CAPSTAN_TRY {
            if(!s.sealed) return portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::invalid));
            bool accepted=s.operation->abort([done](io_result result) noexcept {done.finish(portal::async_result<io_result>(std::move(result)));});
            return accepted ? portal::async_submission::accept() : portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::conflict));
            } CAPSTAN_CATCH_ALL {return portal::async_submission::reject(make_rpc_error(outcome::not_submitted,status::exhausted));}
        });
    }
};
}
