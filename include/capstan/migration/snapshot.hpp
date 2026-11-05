#pragma once
#include <capstan/protocol/types.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include <string>
#include <cstring>

namespace capstan { namespace migration {
using buffer = std::vector<std::uint8_t>;
template<class T> struct snapshot_traits;
constexpr std::size_t max_dataset_bytes = 256u * 1024u * 1024u;
// The snapshot format is independent of compiler layout and generated RPC IDs.
// Header: CPSN, u32 schema=1. Fields: u32 ID, u64 byte length, contents.
// Required IDs: 1 (u64 type=1), 2 (u64 business version), 3 (packed u64 words).
// The high bit marks required fields; unknown optional fields may be skipped.
inline void put(buffer& out, std::uint64_t n, unsigned width) {
    for (unsigned i=0;i<width;++i) out.push_back(static_cast<std::uint8_t>(n>>(8*i)));
}
class reader {
    const std::uint8_t* data_; std::size_t size_,pos_=0;
public:
    reader(const std::uint8_t* data,std::size_t size) noexcept : data_(data),size_(size) {}
    explicit reader(const buffer& b) noexcept : reader(b.data(),b.size()) {}
    const std::uint8_t* current() const noexcept {return data_+pos_;}
    std::size_t remaining() const noexcept {return size_-pos_;}
    bool get(unsigned width,std::uint64_t& out) noexcept {
        if(width>8 || width>remaining()) return false;
        out=0;for(unsigned i=0;i<width;++i) out|=std::uint64_t(data_[pos_++])<<(8*i);
        return true;
    }
    bool text(std::size_t n,std::string& out) {
        if(n>remaining()) return false;
        std::string staged(data_+pos_,data_+pos_+n);
        out=std::move(staged);pos_+=n;return true;
    }
    bool skip(std::size_t n) noexcept {if(n>remaining()) return false;pos_+=n;return true;}
};
struct dataset_snapshot {
    std::uint64_t version=0;
    std::vector<std::uint64_t> words;
};
// Append into a caller-owned, unpublished package. Reserve before writing so
// invalid inputs and allocation failures preserve the existing prefix.
inline status append_snapshot(std::uint64_t version,const std::vector<std::uint64_t>& words,buffer& out) {
    if(words.empty() || words.size()>max_dataset_bytes/8) return status::invalid;
    const std::size_t size=60+words.size()*8;
    if(size>out.max_size()-out.size()) return status::exhausted;
    out.reserve(out.size()+size);
    put(out,0x4e535043u,4);put(out,1,4);
    put(out,0x80000001u,4);put(out,8,8);put(out,1,8);
    put(out,0x80000002u,4);put(out,8,8);put(out,version,8);
    put(out,0x80000003u,4);put(out,words.size()*8,8);
    const std::uint16_t endian=1;
    if(*reinterpret_cast<const std::uint8_t*>(&endian)==1) {
        const auto offset=out.size();out.resize(offset+words.size()*8);
        std::memcpy(out.data()+offset,words.data(),words.size()*8);
    } else {for(auto word:words) put(out,word,8);}
    return status::ok;
}
inline status encode(std::uint64_t version,const std::vector<std::uint64_t>& words,buffer& out) {
    buffer staged;auto code=append_snapshot(version,words,staged);
    if(code==status::ok) out=std::move(staged);
    return code;
}
inline status encode(const dataset_snapshot& snapshot,buffer& out) {return encode(snapshot.version,snapshot.words,out);}
inline status decode(const std::uint8_t* data,std::size_t size,dataset_snapshot& out) {
    if(size>max_dataset_bytes+4096) return status::exhausted;
    reader in(data,size);std::uint64_t magic,schema;
    if(!in.get(4,magic) || magic!=0x4e535043u || !in.get(4,schema) || schema!=1) return status::invalid;
    dataset_snapshot staged;unsigned seen=0,fields=0;
    while(in.remaining()) {
        std::uint64_t tag,length,value;
        if(++fields>64 || !in.get(4,tag) || !in.get(8,length) || length>in.remaining()) return status::invalid;
        auto id=tag&0x7fffffffu;
        if(id>=1 && id<=3) {
            unsigned bit=1u<<static_cast<unsigned>(id-1);
            if(seen&bit) return status::invalid;
            seen|=bit;
            if(id==1) {if(length!=8 || !in.get(8,value) || value!=1) return status::invalid;}
            if(id==2) {if(length!=8 || !in.get(8,staged.version)) return status::invalid;}
            if(id==3) {
                if(!length || length%8 || length>max_dataset_bytes) return status::invalid;
                const std::uint16_t endian=1;
                if(*reinterpret_cast<const std::uint8_t*>(&endian)==1) {
                    staged.words.resize(static_cast<std::size_t>(length/8));
                    std::memcpy(staged.words.data(),in.current(),static_cast<std::size_t>(length));
                    if(!in.skip(static_cast<std::size_t>(length))) return status::invalid;
                } else {
                    staged.words.reserve(static_cast<std::size_t>(length/8));
                    for(std::uint64_t i=0;i<length/8;++i) {if(!in.get(8,value)) return status::invalid;staged.words.push_back(value);}
                }
            }
        } else {
            if(tag&0x80000000u) return status::invalid;
            if(!in.skip(static_cast<std::size_t>(length))) return status::invalid;
        }
    }
    if(seen!=7) return status::invalid;
    out=std::move(staged);return status::ok;
}
inline status decode(const buffer& data,dataset_snapshot& out) {return decode(data.data(),data.size(),out);}
}}
