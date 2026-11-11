#pragma once
#include <capstan/migration/transfer.hpp>
#include <capstan/protocol/wire.hpp>
namespace capstan { namespace migration {
// Borrowed only until the input frame is mutated or destroyed. Consumers must
// finish using this view synchronously, or separately retain the frame owner.
struct chunk_reply_view {std::uint64_t offset=0;const std::uint8_t* data=nullptr;std::size_t size=0;};
// Fast path for status::ok replies with exactly (unsigned offset, binary string).
// False also includes ordinary error replies; use the owning reply decoder for
// their status. Failure leaves out unchanged. It performs no allocation.
inline bool try_decode_chunk_reply(const bytes& frame,chunk_reply_view& out) noexcept {
    namespace w=capstan::wire;
    w::codec::reader input(frame);w::detail::reply_header header;
    if(!w::detail::read_reply_header(input,header) || header.code!=status::ok || header.value_count!=2) return false;
    if(input.integer<std::uint8_t>()!=static_cast<std::uint8_t>(portal::rpc::value_kind::unsigned_integer)) return false;
    const auto offset=input.integer<std::uint64_t>();
    if(input.integer<std::uint8_t>()!=static_cast<std::uint8_t>(portal::rpc::value_kind::string)) return false;
    const auto size=input.integer<std::uint32_t>();
    if(!input.valid() || size!=input.remaining()) return false;
    out={offset,frame.data()+frame.size()-size,size};return true;
}
inline bool try_decode_chunk_reply(bytes&&,chunk_reply_view&)=delete;
inline bool try_decode_chunk_reply(const bytes&&,chunk_reply_view&)=delete;
// New schema namespace; existing grant and business RPC formats are unchanged.
inline std::string encode_manifest(const manifest& m) {
    namespace w=capstan::wire;
    return w::binary(w::command(1810,{w::u(m.transaction.control),w::u(m.transaction.sequence),
        w::u(m.object.cluster),w::u(m.object.incarnation),w::u(m.object.object),
        w::u(m.source_host),w::u(m.target_host),w::u(m.source_instance),w::u(m.target_instance),
        w::u(m.generation),w::u(m.size),w::value::string(std::string(m.digest.begin(),m.digest.end()))}));
}
inline bool decode_manifest(const std::string& data,manifest& out) {
    namespace w=capstan::wire;
    w::codec::request parsed;
    if(!w::try_decode(w::binary(data),parsed) || parsed.method!=1810 || parsed.args.size()!=12) return false;
    for(unsigned i=0;i<11;++i) if(!w::is_number(parsed.args[i])) return false;
    if(!w::is_string(parsed.args[11])) return false;
    auto hash=w::string(parsed.args[11]);if(hash.size()!=32) return false;
    const auto& a=parsed.args;manifest staged;
    staged.transaction={w::number(a[0]),w::number(a[1])};
    staged.object={w::number(a[2]),w::number(a[3]),w::number(a[4])};
    staged.source_host=w::number(a[5]);staged.target_host=w::number(a[6]);
    staged.source_instance=w::number(a[7]);staged.target_instance=w::number(a[8]);
    staged.generation=w::number(a[9]);staged.size=w::number(a[10]);
    if(!staged.transaction.control || !staged.transaction.sequence || !staged.object.cluster || !staged.object.incarnation ||
       !staged.object.object || !staged.source_host || !staged.target_host || !staged.source_instance || !staged.target_instance ||
       !staged.generation || !staged.size || staged.size>512u*1024u*1024u) return false;
    std::copy(hash.begin(),hash.end(),staged.digest.begin());out=staged;return true;
}
}}
