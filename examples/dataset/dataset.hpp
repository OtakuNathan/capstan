#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/bridge/dataset_value.hpp>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>
#include <memory>
#include <cstdlib>
#include <capstan/migration/snapshot.hpp>
// Portal's generated native namespace is nested in portal.
namespace portal { namespace capstan_native {
class dataset {
    std::vector<std::uint64_t> words_;
    std::uint64_t version_=0;
    struct restored {};
    dataset(restored,capstan::migration::dataset_snapshot data)
        : words_(std::move(data.words)),version_(data.version) {}
    enum class add_error { none, range, exhausted };
    // One semantic core for direct calls, transactional publication and redo.
    // Preserve range-before-version error precedence, including empty ranges.
    static add_error validate_add(std::size_t size,std::uint64_t version,
                                  std::uint64_t begin,std::uint64_t end) noexcept {
        if(begin>end || end>size) return add_error::range;
        if(version==std::numeric_limits<std::uint64_t>::max()) return add_error::exhausted;
        return add_error::none;
    }
    static void apply_add(std::vector<std::uint64_t>& words,std::uint64_t& version,
                          std::uint64_t begin,std::uint64_t end,std::uint64_t delta) noexcept {
        for(auto i=begin;i<end;++i) words[static_cast<std::size_t>(i)]+=delta;
        ++version;
    }
public:
    explicit dataset(std::size_t bytes=64u*1024u*1024u, std::uint64_t seed=1) {
        if(bytes==0 || bytes%sizeof(std::uint64_t) || bytes>256u*1024u*1024u)
            CAPSTAN_RAISE(std::invalid_argument("dataset size must be 8-byte aligned, 8 bytes..256 MiB"));
        words_.resize(bytes/sizeof(std::uint64_t));
        // Unsigned arithmetic is explicitly modulo 2^64, including seed stream.
        for(auto& word:words_) { seed=seed*6364136223846793005ULL+1442695040888963407ULL; word=seed; }
    }
    // Caller owns the Host lane and a real admitted read pin throughout capture.
    capstan::migration::dataset_snapshot capture() const {return {version_,words_};}
    capstan::status encode_snapshot(capstan::migration::buffer& out) const {return capstan::migration::encode(version_,words_,out);}
    capstan::status append_snapshot(capstan::migration::buffer& out) const {return capstan::migration::append_snapshot(version_,words_,out);}
    static std::unique_ptr<dataset> restore(capstan::migration::dataset_snapshot data) {
        if(data.words.empty() || data.words.size()>capstan::migration::max_dataset_bytes/8) return {};
        return std::unique_ptr<dataset>(new dataset(restored{},std::move(data)));
    }
    // Transactional backend only: prepare under the exclusive object lane,
    // persist the resulting effect, then publish without allocation or throwing.
    struct prepared_add {std::uint64_t before=0,begin=0,end=0,delta=0;};
    bool prepare_add(std::uint64_t begin,std::uint64_t end,std::uint64_t delta,prepared_add& out) const noexcept {
        if(!can_add_range(begin,end)) return false;
        out={version_,begin,end,delta};return true;
    }
    void publish_add(const prepared_add& p) noexcept {
        // A mismatch is a broken backend serialization contract, never a
        // recoverable rejection after the durable effect has committed.
        if(version_!=p.before || !can_add_range(p.begin,p.end)) std::abort();
        apply_add(words_,version_,p.begin,p.end,p.delta);
    }
    std::uint64_t version() const noexcept { return version_; }
    std::size_t resource_bytes() const noexcept { return words_.size()*sizeof(std::uint64_t); }
    capstan::summary_t summary() const noexcept {
        capstan::summary_t out{static_cast<std::uint64_t>(words_.size()),0,0,version_};
        for(std::size_t i=0;i<words_.size();++i) { out.sum+=words_[i]; out.checksum^=words_[i]+static_cast<std::uint64_t>(i); }
        return out;
    }
    bool can_add_range(std::uint64_t begin,std::uint64_t end) const noexcept {
        return validate_add(words_.size(),version_,begin,end)==add_error::none;
    }
    std::uint64_t add_range(std::uint64_t begin,std::uint64_t end,std::uint64_t delta) {
        const auto error=validate_add(words_.size(),version_,begin,end);
        if(error==add_error::range) CAPSTAN_RAISE(std::out_of_range("invalid dataset range"));
        if(error==add_error::exhausted) CAPSTAN_RAISE(std::overflow_error("dataset version exhausted"));
        apply_add(words_,version_,begin,end,delta);
        return version_;
    }
    // Recovering a staged snapshot is fallible; never abort on malformed disk
    // input. Format, ordering and receipt authentication remain backend concerns.
    static bool replay_add(capstan::migration::dataset_snapshot& data,const prepared_add& p) noexcept {
        if(data.version!=p.before || validate_add(data.words.size(),data.version,p.begin,p.end)!=add_error::none) return false;
        apply_add(data.words,data.version,p.begin,p.end,p.delta);
        return true;
    }

};
}}

namespace capstan { namespace migration {
template<> struct snapshot_traits<portal::capstan_native::dataset> {
    using object_type= portal::capstan_native::dataset;
    static status capture(const object_type& object,buffer& out) {return object.encode_snapshot(out);}
    static status restore(const buffer& data,std::unique_ptr<object_type>& out) {
        dataset_snapshot snapshot;auto code=decode(data,snapshot);
        if(code!=status::ok) return code;
        auto staged=object_type::restore(std::move(snapshot));
        if(!staged) return status::invalid;
        out=std::move(staged);return status::ok;
    }
};
}}
