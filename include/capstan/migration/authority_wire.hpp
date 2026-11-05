#pragma once
#include <capstan/migration/authority.hpp>
#include <capstan/migration/snapshot.hpp>
#include <capstan/protocol/wire.hpp>
namespace capstan { namespace migration {
constexpr id freeze_command=4100,query_command=4101,fence_command=4102,abort_command=4103,host_info_command=4104,export_command=4105,export_chunk_command=4106,export_release_command=4107,reserve_export_command=4108;
constexpr id reserve_command=4110,pull_command=4111,import_query_command=4112,import_abort_command=4113,activate_command=4114,retire_command=4115;
inline std::string encode_ticket(const ticket& t) {
    namespace w=capstan::wire;
    return w::binary(w::command(1812,{w::u(t.control),w::u(t.sequence),w::u(t.source_host),w::u(t.source_instance),
        w::u(t.target_host),w::u(t.target_instance),w::u(t.generation),w::value::string(w::encode_grant(t.source))}));
}
inline bool decode_ticket(const std::string& data,ticket& out) {
    namespace w=capstan::wire;w::codec::request r;
    if(!w::try_decode(w::binary(data),r) || r.method!=1812 || r.args.size()!=8) return false;
    for(unsigned i=0;i<7;++i) if(!w::is_number(r.args[i]) || !w::number(r.args[i])) return false;
    ticket t;auto& a=r.args;t.control=w::number(a[0]);t.sequence=w::number(a[1]);t.source_host=w::number(a[2]);
    t.source_instance=w::number(a[3]);t.target_host=w::number(a[4]);t.target_instance=w::number(a[5]);t.generation=w::number(a[6]);
    if(!w::is_string(a[7]) || !w::try_decode_grant(w::string(a[7]),t.source) || t.source.mode!=access_mode::exclusive ||
       t.source_host==t.target_host || t.generation==std::numeric_limits<id>::max() || t.source.epoch==std::numeric_limits<id>::max()) return false;
    out=t;return true;
}
inline void write_string(buffer& out,const std::string& s) {put(out,s.size(),8);out.insert(out.end(),s.begin(),s.end());}
inline bool read_string(reader& in,std::string& s) {
    std::uint64_t n;if(!in.get(8,n) || n>capstan::wire::max_frame || n>in.remaining()) return false;
    return in.text(static_cast<std::size_t>(n),s);
}
inline status encode_authority_image(const authority_image& image,buffer& out,std::size_t history_limit=4096) {
    namespace w=capstan::wire;
    if(image.requests.size()>history_limit || image.transactions.size()>history_limit || image.migrations.size()>history_limit) return status::exhausted;
    buffer b;put(b,0x55415043u,4);put(b,1,4);
    for(auto n:{image.key.cluster,image.key.incarnation,image.key.object,image.control,image.epoch,image.generation}) put(b,n,8);
    put(b,image.requests.size(),8);
    for(const auto& r:image.requests) {if(!r.second) return status::invalid;put(b,r.first.first,8);put(b,r.first.second,8);}
    put(b,image.transactions.size(),8);
    for(const auto& r:image.transactions) {
        write_string(b,w::binary(w::encode_transition(r.second.target)));
        write_string(b,r.second.target.origin==authority_origin::holder_transfer ? w::encode_transfer_request(r.second.source) : std::string{});
    }
    put(b,image.migrations.size(),8);
    for(const auto& r:image.migrations) {
        if(r.first!=r.second.request.sequence || r.second.result_grants.size()>1) return status::invalid;
        write_string(b,encode_ticket(r.second.request));put(b,static_cast<unsigned>(r.second.state),4);
        put(b,r.second.result_grants.size(),4);
        if(!r.second.result_grants.empty()) write_string(b,w::encode_grant(r.second.result_grants.front()));
    }
    out=std::move(b);return status::ok;
}
inline status decode_authority_image(const std::uint8_t* bytes,std::size_t size,authority_image& out,std::size_t history_limit=4096) {
    namespace w=capstan::wire;
    if(size>512u*1024u*1024u) return status::exhausted;
    reader in(bytes,size);std::uint64_t magic,version,count;authority_image staged;
    if(!in.get(4,magic) || magic!=0x55415043u || !in.get(4,version) || version!=1) return status::invalid;
    for(auto* n:{&staged.key.cluster,&staged.key.incarnation,&staged.key.object,&staged.control,&staged.epoch,&staged.generation})
        if(!in.get(8,*n) || !*n) return status::invalid;
    if(!in.get(8,count) || count>history_limit || count>in.remaining()/16) return status::invalid;
    for(std::uint64_t i=0;i<count;++i) {
        id holder,request;if(!in.get(8,holder) || !in.get(8,request) || !holder || !request ||
            !staged.requests.emplace(std::make_pair(holder,request),true).second) return status::invalid;
    }
    if(!in.get(8,count) || count>history_limit || count>in.remaining()/16) return status::invalid;
    for(std::uint64_t i=0;i<count;++i) {
        std::string target,source;control_record r;
        if(!read_string(in,target) || !read_string(in,source) || !w::try_decode_transition(w::binary(target),r.target) ||
           r.target.key!=staged.key || r.target.control_session!=staged.control || r.target.next_epoch>staged.epoch ||
           validate_target(staged.key,r.target.next_epoch,r.target.destroy,r.target.grants)!=status::ok) return status::invalid;
        if(r.target.origin==authority_origin::holder_transfer) {
            if(!w::try_decode_transfer_request(source,r.source) || r.source.source.key!=staged.key || r.source.transaction!=r.target.identity()) return status::invalid;
        } else if(!source.empty()) return status::invalid;
        if(!staged.transactions.emplace(r.target.identity(),std::move(r)).second) return status::invalid;
    }
    if(!in.get(8,count) || count>history_limit || count>in.remaining()/16) return status::invalid;
    for(std::uint64_t i=0;i<count;++i) {
        std::string data;authority_record r;std::uint64_t phase,rollbacks;
        if(!read_string(in,data) || !decode_ticket(data,r.request) || r.request.control!=staged.control || r.request.source.key!=staged.key ||
           !in.get(4,phase) || phase>static_cast<unsigned>(authority_phase::installed) || !in.get(4,rollbacks) || rollbacks>1) return status::invalid;
        r.state=static_cast<authority_phase>(phase);
        if(rollbacks) {
            grant g;if(!read_string(in,data) || !w::try_decode_grant(data,g) || g.key!=staged.key) return status::invalid;
            r.result_grants.push_back(g);
        }
        if(!staged.migrations.emplace(r.request.sequence,std::move(r)).second) return status::invalid;
    }
    if(in.remaining()) return status::invalid;
    out=std::move(staged);return status::ok;
}
inline status decode_authority_image(const buffer& bytes,authority_image& out,std::size_t history_limit=4096) {return decode_authority_image(bytes.data(),bytes.size(),out,history_limit);}
}}
