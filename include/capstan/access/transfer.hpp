#pragma once
#include <capstan/access/capability.hpp>
namespace capstan {
// Preparation is local and inert. The source is sealed only when FF actually
// reaches endpoint submission, including submissions whose outcome is unknown.
class transfer_plan {
    struct operation {
        const transfer_request request;
        const bytes packet;
        std::shared_ptr<access_detail::exclusive_state> owner;
        std::atomic<bool> submitted{false};
        operation(transfer_request r,std::shared_ptr<access_detail::exclusive_state> o):request(r),packet(wire::command(wire::transfer_exclusive,{wire::value::string(wire::encode_transfer_request(r))})),owner(std::move(o)) {}
    };
    std::shared_ptr<operation> operation_;
    explicit transfer_plan(std::shared_ptr<operation> op):operation_(std::move(op)) {}
    struct sealing_endpoint final:io_endpoint {
        io_endpoint& transport;
        std::shared_ptr<operation> operation_;
        sealing_endpoint(io_endpoint& e,std::shared_ptr<operation> op):transport(e),operation_(std::move(op)) {}
        bool submit(bytes packet,io_completion done) noexcept override {
            // Keep state independent of this adapter across inline completion.
            auto op=operation_;auto* target=&transport;
            if(!access_detail::seal(op->owner)) return false;
            op->submitted.store(true);
            return target->submit_owned(std::move(packet),std::move(done),op);
        }
    };
public:
    static std::unique_ptr<transfer_plan> prepare(const exclusive_capability& source,id recipient,id sequence,status& code) {
        code=status::invalid;
        if(!source.owner_ || !sequence || sequence==std::numeric_limits<id>::max() ||
           (recipient!=1 && recipient!=2) || recipient==source.owner_->identity.holder) return {};
        {
            std::lock_guard<std::mutex> lock(source.owner_->mutex);
            if(source.owner_->closed) {code=status::closed;return {};}
        }
        transfer_request request{source.owner_->identity,{authority_origin::holder_transfer,source.owner_->identity.holder,sequence},recipient};
        auto op=std::make_shared<operation>(request,source.owner_);
        auto result=std::unique_ptr<transfer_plan>(new transfer_plan(std::move(op)));
        code=status::ok;return result;
    }
    const transfer_request& request() const noexcept {return operation_->request;}
    bool sealed() const noexcept {return operation_->submitted.load();}
    void start(io_endpoint& control,io_completion done) const {
        auto adapter=std::make_shared<sealing_endpoint>(control,operation_);
        auto packet=operation_->packet;
        auto* endpoint=adapter.get();
        start_io(*endpoint,std::move(packet),std::move(done),std::move(adapter));
    }
    // Explicit retries preserve the exact original transaction and source.
    // A retry cannot restore local Live, even on Rejected or NotSeen.
    void retry(io_endpoint& control,io_completion done) const {
        if(!sealed()) {done({outcome::not_submitted,{}});return;}
        start_io(control,operation_->packet,std::move(done),operation_);
    }
};
struct transfer_query_result {
    outcome state=outcome::unknown;
    status code=status::invalid;
    transfer_receipt receipt;
};
inline void query_transfer(io_endpoint& control,transfer_ref reference,std::function<void(transfer_query_result)> done) {
    start_io(control,wire::command(wire::query_transfer,{wire::value::string(wire::encode_transfer_ref(reference))}),
        [reference,done=std::move(done)](io_result io) mutable {
            transfer_query_result result;result.state=io.state;
            wire::reply reply;
            if(io.state==outcome::completed) {
                if(!wire::try_decode_reply(io.payload,reply)) result.state=outcome::unknown;
                else {
                    result.state=reply.state;result.code=reply.code;
                    if(reply.values.size()!=1 || !wire::is_string(reply.values[0]) ||
                       !wire::try_decode_transfer_receipt(wire::string(reply.values[0]),result.receipt) ||
                       !(result.receipt.reference==reference) || result.receipt.code!=reply.code) {
                        result.state=outcome::unknown;result.code=status::invalid;
                    }
                }
            }
            done(std::move(result));
        });
}
struct transfer_claim_result {
    outcome state=outcome::unknown;
    status code=status::invalid;
    grant metadata;
    std::unique_ptr<exclusive_capability> capability;
};
inline void claim_transfer(io_endpoint& control,transfer_ref reference,std::function<void(transfer_claim_result)> done) {
    start_io(control,wire::command(wire::claim_transfer,{wire::value::string(wire::encode_transfer_ref(reference))}),
        [reference,done=std::move(done)](io_result io) mutable {
            transfer_claim_result result;result.state=io.state;wire::reply reply;
            if(io.state==outcome::completed) {
                if(!wire::try_decode_reply(io.payload,reply)) result.state=outcome::unknown;
                else {
                    result.state=reply.state;result.code=reply.code;
                    if(reply.code==status::ok) {
                        auto& grant=result.metadata;
                        if(reply.state!=outcome::completed || reply.values.size()!=1 || !wire::is_string(reply.values[0]) ||
                           !wire::try_decode_grant(wire::string(reply.values[0]),grant) || grant.key!=reference.key ||
                           grant.holder!=reference.to_holder || reference.source_epoch==std::numeric_limits<id>::max() ||
                           grant.epoch!=reference.source_epoch+1 || grant.mode!=access_mode::exclusive) {
                            result.state=outcome::unknown;result.code=status::invalid;
                        } else {
                            result.capability=exclusive_capability::try_create(grant,result.code);
                            if(!result.capability) result.state=outcome::rejected;
                        }
                    }
                }
            }
            done(std::move(result));
        });
}

}
