#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/bridge/dataset_value.hpp>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>
// Dynabridge's generated native namespace is nested in dynabridge.
namespace dynabridge { namespace capstan_native {
class dataset {
    std::vector<std::uint64_t> words_;
    std::uint64_t version_=0;
public:
    explicit dataset(std::size_t bytes=64u*1024u*1024u, std::uint64_t seed=1) {
        if(bytes==0 || bytes%sizeof(std::uint64_t) || bytes>256u*1024u*1024u)
            CAPSTAN_RAISE(std::invalid_argument("dataset size must be 8-byte aligned, 8 bytes..256 MiB"));
        words_.resize(bytes/sizeof(std::uint64_t));
        // Unsigned arithmetic is explicitly modulo 2^64, including seed stream.
        for(auto& word:words_) { seed=seed*6364136223846793005ULL+1442695040888963407ULL; word=seed; }
    }
    std::uint64_t version() const noexcept { return version_; }
    std::size_t resource_bytes() const noexcept { return words_.size()*sizeof(std::uint64_t); }
    capstan::summary_t summary() const noexcept {
        capstan::summary_t out{static_cast<std::uint64_t>(words_.size()),0,0,version_};
        for(std::size_t i=0;i<words_.size();++i) { out.sum+=words_[i]; out.checksum^=words_[i]+static_cast<std::uint64_t>(i); }
        return out;
    }
    bool can_add_range(std::uint64_t begin,std::uint64_t end) const noexcept {
        return begin<=end && end<=words_.size() && version_!=std::numeric_limits<std::uint64_t>::max();
    }
    std::uint64_t add_range(std::uint64_t begin,std::uint64_t end,std::uint64_t delta) {
        if(begin>end || end>words_.size()) CAPSTAN_RAISE(std::out_of_range("invalid dataset range"));
        if(version_==std::numeric_limits<std::uint64_t>::max()) CAPSTAN_RAISE(std::overflow_error("dataset version exhausted"));
        for(auto i=begin;i<end;++i) words_[static_cast<std::size_t>(i)]+=delta;
        return ++version_;
    }
};
}}
