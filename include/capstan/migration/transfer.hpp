#pragma once
#include <capstan/migration/snapshot.hpp>
#include <algorithm>
#include <array>
#include <memory>
#include <capstan/error_policy.hpp>

namespace capstan { namespace migration {
// All methods run in the owning Host's serialized decision domain. Identity is
// checked after authentication by the adapter, before calling these methods.
struct transaction_id {
    id control=0,sequence=0;
    bool operator==(const transaction_id& b) const noexcept {return control==b.control && sequence==b.sequence;}
};
struct manifest {
    transaction_id transaction;
    object_key object;
    id source_host=0,target_host=0,source_instance=0,target_instance=0,generation=0;
    std::uint64_t size=0;
    std::array<std::uint8_t,32> digest{};
    bool operator==(const manifest& b) const noexcept {
        return transaction==b.transaction && object==b.object && source_host==b.source_host && target_host==b.target_host &&
            source_instance==b.source_instance && target_instance==b.target_instance && generation==b.generation && size==b.size && digest==b.digest;
    }
};
struct transfer_digest {
    virtual ~transfer_digest() = default;
    virtual void update(const std::uint8_t*,std::size_t) noexcept = 0;
    virtual std::array<std::uint8_t,32> value() noexcept = 0;
};
class transfer_receiver {
public:
    enum class phase {empty,receiving,prepared,aborted};
    static constexpr std::size_t chunk_bytes=48u*1024u;
private:
    manifest identity_{};
    phase phase_=phase::empty;
    std::shared_ptr<buffer> storage_;
    std::size_t capacity_,received_=0,prefix_=0;
    std::vector<bool> chunks_;
    std::unique_ptr<transfer_digest> digest_;
    bool digest_valid_=false;
public:
    explicit transfer_receiver(std::size_t capacity=512u*1024u*1024u,std::unique_ptr<transfer_digest> digest={}) noexcept : capacity_(capacity),digest_(std::move(digest)) {}
    transfer_receiver(const transfer_receiver&)=delete;
    transfer_receiver& operator=(const transfer_receiver&)=delete;
    phase state() const noexcept {return phase_;}
    const manifest& identity() const noexcept {return identity_;}
    std::size_t received() const noexcept {return received_;}
    std::size_t contiguous_bytes() const noexcept {return prefix_;}
    bool has_chunk(std::size_t offset) const noexcept {return offset<identity_.size && offset%chunk_bytes==0 && offset/chunk_bytes<chunks_.size() && chunks_[offset/chunk_bytes];}
    bool digest_valid() const noexcept {return digest_valid_;}
    status begin(const manifest& m) {
        if(phase_!=phase::empty) {
            if(!(identity_==m)) return status::conflict;
            return phase_==phase::aborted ? status::closed : status::ok;
        }
        if(!m.transaction.control || !m.transaction.sequence || !m.source_host || !m.target_host ||
           !m.source_instance || !m.target_instance || !m.generation || !m.object.cluster || !m.object.incarnation || !m.object.object || !m.size)
            return status::invalid;
        if(m.size>capacity_) return status::exhausted;
        auto staged=std::make_shared<buffer>(static_cast<std::size_t>(m.size));
        std::vector<bool> chunks((static_cast<std::size_t>(m.size)-1)/chunk_bytes+1,false);
        chunks_=std::move(chunks);
        identity_=m;storage_=std::move(staged);phase_=phase::receiving;return status::ok;
    }
    status append(const manifest& m,std::uint64_t offset,const buffer& data) {
        return append(m,offset,data.data(),data.size());
    }
    status append(const manifest& m,std::uint64_t offset,const std::uint8_t* data,std::size_t size) {
        if(phase_==phase::empty) return status::not_seen;
        if(!(identity_==m)) return status::conflict;
        if(phase_!=phase::receiving) return status::closed;
        if(!data || offset>=m.size || offset%chunk_bytes || size!=std::min<std::uint64_t>(std::uint64_t(chunk_bytes),m.size-offset)) return status::invalid;
        const auto at=static_cast<std::size_t>(offset),index=at/chunk_bytes;
        if(chunks_[index]) return std::equal(data,data+size,storage_->data()+at) ? status::ok : status::conflict;
        std::copy(data,data+size,storage_->data()+at);chunks_[index]=true;received_+=size;
        const auto previous=prefix_;
        while(prefix_<m.size && chunks_[prefix_/chunk_bytes]) prefix_+=std::min(std::size_t(chunk_bytes),static_cast<std::size_t>(m.size)-prefix_);
        if(digest_ && prefix_>previous) {
            digest_->update(storage_->data()+previous,prefix_-previous);
            if(prefix_==m.size) digest_valid_=digest_->value()==m.digest;
        }
        return status::ok;
    }
    // The backend supplies SHA-256 validation AND object/schema restoration.
    // No authority can be installed by a successful Seal alone.
    template<class Validate> status seal(const manifest& m,Validate&& validate) {
#if !CAPSTAN_HAS_EXCEPTIONS && (!defined(CAPSTAN_NO_EXCEPTION_STRICT) || CAPSTAN_NO_EXCEPTION_STRICT)
        static_assert(noexcept(validate(std::declval<const buffer&>(),std::declval<const std::array<std::uint8_t,32>&>())),
                      "strict no-exception validation must be noexcept");
#endif
        if(phase_==phase::empty) return status::not_seen;
        if(!(identity_==m)) return status::conflict;
        if(phase_==phase::aborted) return status::closed;
        if(phase_==phase::prepared) return status::ok;
        if(received_!=m.size) return status::pending;
        if(digest_ && !digest_valid_) return status::invalid;
        auto retained=storage_;
        auto code=validate(static_cast<const buffer&>(*retained),m.digest);
        // Validation may reenter Abort. Retain its input and never resurrect a
        // tombstone when the callback returns.
        if(phase_!=phase::receiving || storage_!=retained) return status::closed;
        if(code!=status::ok) return code;
        phase_=phase::prepared;return status::ok;
    }
    // Even an Abort arriving before Begin leaves a tombstone. Existing readers
    // own their buffer independently; aborting an observer cannot free it early.
    status abort(const manifest& m) noexcept {
        if(phase_!=phase::empty && !(identity_==m)) return status::conflict;
        identity_=m;phase_=phase::aborted;storage_.reset();chunks_.clear();received_=prefix_=0;digest_valid_=false;digest_.reset();return status::ok;
    }
    // Backend-only cleanup after restoration has finished using the bytes.
    // Identity and Prepared evidence remain available for idempotent controls.
    void release_prepared_storage() noexcept {if(phase_==phase::prepared) storage_.reset();}
    std::shared_ptr<const buffer> prepared_data() const noexcept {
        return phase_==phase::prepared ? storage_ : nullptr;
    }
};
}}
