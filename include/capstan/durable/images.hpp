#pragma once
#include <capstan/authority/host.hpp>
#include <capstan/authority/directory.hpp>
#include <capstan/protocol/wire.hpp>
#include <capstan/migration/snapshot.hpp>
namespace capstan { namespace durable {
// Private, versioned storage envelopes. They do not change RPC method IDs.
struct image_writer {
    bytes value;
    explicit image_writer(id magic) {number(magic);number(1);}
    void number(id n) {migration::put(value,n,8);}
    void blob(const bytes& b) {number(b.size());value.insert(value.end(),b.begin(),b.end());}
    void text(const std::string& s) {blob(wire::binary(s));}
};
struct image_reader {
    migration::reader in;bool valid;
    image_reader(const bytes& b,id magic):in(b),valid(false) {id m,v;valid=in.get(8,m)&&in.get(8,v)&&m==magic&&v==1;}
    bool number(id& n) {return valid && (valid=in.get(8,n));}
    bool blob(bytes& b,std::size_t limit=migration::max_dataset_bytes+4096) {
        id n;if(!number(n) || n>limit || n>in.remaining()) {valid=false;return false;}
        b.assign(in.current(),in.current()+static_cast<std::size_t>(n));return valid=in.skip(static_cast<std::size_t>(n));
    }
    bool text(std::string& s,std::size_t limit=wire::max_frame) {bytes b;if(!blob(b,limit)) return false;s=wire::binary(b);return true;}
    bool end() const noexcept {return valid && !in.remaining();}
};
inline bytes encode_host(const host_authority::durable_image& i) {
    image_writer w(0x31485443);w.number(i.key.cluster);w.number(i.key.incarnation);w.number(i.key.object);
    w.number(i.control);w.number(i.epoch);w.number(static_cast<unsigned>(i.gate));w.number(i.pending.sequence);
    w.number(i.grants.size());for(const auto& g:i.grants) w.text(wire::encode_grant(g));
    w.number(i.transactions.size());for(const auto& t:i.transactions) w.blob(wire::encode_transition(t.second));
    return std::move(w.value);
}
inline bool decode_host(const bytes& b,host_authority::durable_image& out,std::size_t limit) {
    image_reader r(b,0x31485443);host_authority::durable_image i;id gate,n,pending;
    if(!r.number(i.key.cluster)||!r.number(i.key.incarnation)||!r.number(i.key.object)||!r.number(i.control)||!r.number(i.epoch)||
       !r.number(gate)||gate>2||!r.number(pending)||!r.number(n)||n>64) return false;
    i.gate=static_cast<host_authority::phase>(gate);if(pending) i.pending={authority_origin::directory,i.control,pending};
    for(id a=0;a<n;++a) {std::string s;grant g;if(!r.text(s)||!wire::try_decode_grant(s,g)) return false;i.grants.push_back(g);}
    if(!r.number(n)||n>limit) return false;
    for(id a=0;a<n;++a) {bytes v;transition t;if(!r.blob(v,wire::max_frame)||!wire::try_decode_transition(v,t)||!i.transactions.emplace(t.identity(),t).second) return false;}
    if(!r.end()) return false;
    out=std::move(i);return true;
}
inline void encode_directory(image_writer& w,const directory_entry::durable_image& i) {
    w.number(i.epoch);w.number(i.next_tx);w.number(i.pending);w.number(i.dead);w.number(i.ready);w.number(i.current.tx!=0);
    if(i.current.tx) w.blob(wire::encode_transition(i.current));
}
inline bool decode_directory(image_reader& r,directory_entry::durable_image& i) {
    id p,d,ready,has;
    if(!r.number(i.epoch)||!r.number(i.next_tx)||!r.number(p)||p>1||!r.number(d)||d>1||!r.number(ready)||ready>1||!r.number(has)||has>1) return false;
    i.pending=p;i.dead=d;i.ready=ready;
    if(has) {bytes v;if(!r.blob(v,wire::max_frame)||!wire::try_decode_transition(v,i.current)) return false;}return true;
}
}}
