#pragma once
#include <capstan/migration/wire.hpp>
namespace capstan { namespace migration {
constexpr id copy_command=4200,copy_chunk_command=4201,copy_release_command=4202,copy_query_command=4203;
constexpr id clone_command=4210,clone_query_command=4211,clone_abort_command=4212;
constexpr id route_command=4900;
struct copy_ref {object_key source;id holder=0,sequence=0;};
// Public, caller-bound snapshot identity. No grant secret, native pointer or
// authority history is included. Possession alone does not authorize access.
struct snapshot_ref {
    object_key source;
    id host=0,instance=0,holder=0,sequence=0;
    std::uint64_t size=0;
    std::array<std::uint8_t,32> digest{};
    bool operator==(const snapshot_ref& b) const noexcept {return source==b.source && host==b.host && instance==b.instance && holder==b.holder && sequence==b.sequence && size==b.size && digest==b.digest;}
};
inline std::string encode_snapshot_ref(const snapshot_ref& r) {
    namespace w=capstan::wire;
    return w::binary(w::command(1820,{w::u(r.source.cluster),w::u(r.source.incarnation),w::u(r.source.object),w::u(r.host),w::u(r.instance),
        w::u(r.holder),w::u(r.sequence),w::u(r.size),w::value::string(std::string(r.digest.begin(),r.digest.end()))}));
}
inline bool decode_snapshot_ref(const std::string& encoded,snapshot_ref& out) {
    namespace w=capstan::wire;w::codec::request r;
    if(!w::try_decode(w::binary(encoded),r) || r.method!=1820 || r.args.size()!=9) return false;
    for(unsigned i=0;i<8;++i) if(!w::is_number(r.args[i]) || !w::number(r.args[i])) return false;
    if(!w::is_string(r.args[8])) return false;
    auto hash=w::string(r.args[8]);if(hash.size()!=32) return false;
    auto& a=r.args;snapshot_ref staged;
    staged.source={w::number(a[0]),w::number(a[1]),w::number(a[2])};staged.host=w::number(a[3]);staged.instance=w::number(a[4]);
    staged.holder=w::number(a[5]);staged.sequence=w::number(a[6]);staged.size=w::number(a[7]);
    if(staged.holder>2 || staged.size>max_dataset_bytes+4096) return false;
    std::copy(hash.begin(),hash.end(),staged.digest.begin());out=staged;return true;
}
inline bytes route(object_key key,bytes payload) {
    return capstan::wire::command(route_command,{capstan::wire::u(key.cluster),capstan::wire::u(key.incarnation),capstan::wire::u(key.object),capstan::wire::value::string(capstan::wire::binary(payload))});
}
}}
