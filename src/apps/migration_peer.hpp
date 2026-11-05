#pragma once
#include "common.hpp"
#include "migration_metrics.hpp"
#include <capstan/migration/authority_wire.hpp>
#include <capstan/migration/wire.hpp>
#include <capstan/migration/clipboard.hpp>
namespace app {
inline gio_limits transfer_limits() noexcept {gio_limits limits;limits.max_inflight=4;return limits;}
// Four accepted exchanges at most. The receiver owns offsets, duplicate checks
// and ordered digest progress; transport callbacks retain this operation until
// every accepted exchange has completed, even after the first failure.
class chunk_pull : public std::enable_shared_from_this<chunk_pull> {
    std::shared_ptr<gio_client> socket_;
    authenticated_endpoint endpoint_;
    migration::manifest manifest_;
    std::shared_ptr<migration::transfer_receiver> receiver_;
    object_key source_;
    id command_;
    std::string binding_;
    std::function<void(status)> completion_;
    std::shared_ptr<transfer_metrics> metrics_;
    std::size_t next_=0,inflight_=0;
    status result_=status::ok;
    bool pumping_=false;
    void receive(std::size_t offset,io_result io) noexcept {
        --inflight_;
        CAPSTAN_TRY {
            if(result_==status::ok) {
                if(receiver_->state()==migration::transfer_receiver::phase::aborted) {result_=status::closed;start();return;}
                migration::chunk_reply_view chunk;
                const auto decoding=metric_stamp::now(bool(metrics_));
                const bool decoded=io.state==outcome::completed && migration::try_decode_chunk_reply(io.payload,chunk);
                if(metrics_) metrics_->decode.add(decoding,metric_stamp::now());
                if(!decoded) {
                    // Keep existing distinctions between malformed transport,
                    // a valid error status and a valid but wrong result shape.
                    wire::reply r;
                    if(io.state!=outcome::completed || !wire::try_decode_reply(io.payload,r)) result_=status::pending;
                    else result_=r.code!=status::ok ? r.code : status::invalid;
                }
                else if(chunk.offset!=offset) result_=status::invalid;
                else {
                    // io.payload owns these bytes until this synchronous append
                    // copies them into receiver storage. No view is retained.
                    const auto appending=metric_stamp::now(bool(metrics_));
                    result_=receiver_->append(manifest_,offset,chunk.data,chunk.size);
                    if(metrics_) metrics_->append.add(appending,metric_stamp::now());
                }
            }
        } CAPSTAN_CATCH_ALL {result_=status::exhausted;}
        start();
    }
public:
    chunk_pull(std::shared_ptr<gio_client> socket,token credential,migration::manifest manifest,
               std::shared_ptr<migration::transfer_receiver> receiver,object_key source,id command,
               std::string binding,std::function<void(status)> done,std::shared_ptr<transfer_metrics> metrics={})
        :socket_(std::move(socket)),endpoint_(*socket_,credential),manifest_(manifest),receiver_(std::move(receiver)),
         source_(source),command_(command),binding_(std::move(binding)),completion_(std::move(done)),metrics_(std::move(metrics)) {}
    void start() noexcept {
        if(pumping_ || !completion_) return;
        pumping_=true;
        CAPSTAN_TRY {
            if(receiver_->state()==migration::transfer_receiver::phase::aborted) result_=status::closed;
            while(result_==status::ok && inflight_<4 && next_<manifest_.size) {
                const auto offset=next_;next_+=std::min(std::size_t(migration::transfer_receiver::chunk_bytes),static_cast<std::size_t>(manifest_.size)-next_);
                if(receiver_->has_chunk(offset)) continue; // Same-binding retry retains valid chunks.
                auto packet=migration::route(source_,wire::command(command_,{wire::value::string(binding_),wire::u(offset)}));
                auto self=shared_from_this();++inflight_;
                if(!endpoint_.submit(std::move(packet),[self,offset](io_result io) noexcept {self->receive(offset,std::move(io));})) {--inflight_;result_=status::rejected;}
            }
        } CAPSTAN_CATCH_ALL {result_=status::exhausted;}
        pumping_=false;
        if(!inflight_ && (result_!=status::ok || next_==manifest_.size)) {
            if(result_==status::ok && receiver_->received()!=manifest_.size) result_=status::invalid;
            auto done=std::exchange(completion_,nullptr);
            if(done) {CAPSTAN_TRY {done(result_);} CAPSTAN_CATCH_ALL {}}
        }
    }
};
class migration_pull : public std::enable_shared_from_this<migration_pull> {
    std::shared_ptr<gio_client> socket_;
    authenticated_endpoint endpoint_;
    token credential_;
    migration::ticket ticket_;
    migration::manifest manifest_;
    std::shared_ptr<migration::transfer_receiver> receiver_;
    std::function<void(status,const migration::manifest&)> completion_;
    std::shared_ptr<transfer_metrics> metrics_;
    void finish(status code) noexcept {auto done=std::exchange(completion_,nullptr);if(done) {CAPSTAN_TRY {done(code,manifest_);} CAPSTAN_CATCH_ALL {}}}
public:
    migration_pull(std::shared_ptr<gio_client> socket,token credential,migration::ticket t,std::shared_ptr<migration::transfer_receiver> receiver,
                   std::function<void(status,const migration::manifest&)> done,std::shared_ptr<transfer_metrics> metrics={})
        :socket_(std::move(socket)),endpoint_(*socket_,credential),credential_(credential),ticket_(t),receiver_(std::move(receiver)),completion_(std::move(done)),metrics_(std::move(metrics)) {}
    void start() noexcept {
        CAPSTAN_TRY {
            auto self=shared_from_this();
            auto packet=migration::route(ticket_.source.key,wire::command(migration::export_command,{wire::value::string(migration::encode_ticket(ticket_))}));
            if(!endpoint_.submit(std::move(packet),[self](io_result io) noexcept {
                CAPSTAN_TRY {
                    if(self->receiver_->state()==migration::transfer_receiver::phase::aborted) {self->finish(status::closed);return;}
                    wire::reply r;
                    if(io.state!=outcome::completed || !wire::try_decode_reply(io.payload,r)) {self->finish(status::pending);return;}
                    if(r.code!=status::ok) {self->finish(r.code);return;}
                    if(r.values.size()!=1 || !wire::is_string(r.values[0]) || !migration::decode_manifest(wire::string(r.values[0]),self->manifest_) || !migration::matches(self->manifest_,self->ticket_)) {self->finish(status::invalid);return;}
                    auto code=self->receiver_->begin(self->manifest_);if(code!=status::ok) {self->finish(code);return;}
                    auto pull=std::make_shared<chunk_pull>(self->socket_,self->credential_,self->manifest_,self->receiver_,self->ticket_.source.key,migration::export_chunk_command,migration::encode_ticket(self->ticket_),[self](status code) noexcept {self->finish(code);},self->metrics_);
                    pull->start();
                } CAPSTAN_CATCH_ALL {self->finish(status::exhausted);}
            })) finish(status::rejected);
        } CAPSTAN_CATCH_ALL {finish(status::exhausted);}
    }
};
class snapshot_pull {
    std::shared_ptr<chunk_pull> pull_;
public:
    snapshot_pull(std::shared_ptr<gio_client> socket,token credential,migration::snapshot_ref reference,migration::manifest manifest,
                  std::shared_ptr<migration::transfer_receiver> receiver,std::function<void(status)> done)
        :pull_(std::make_shared<chunk_pull>(std::move(socket),credential,manifest,std::move(receiver),reference.source,migration::copy_chunk_command,migration::encode_snapshot_ref(reference),std::move(done))) {}
    void start() noexcept {pull_->start();}
};
}
