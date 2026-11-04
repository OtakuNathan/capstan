#pragma once
#include <capstan/transport/io.hpp>
#include <limits>
#include <capstan/error_policy.hpp>
#include <stdexcept>
namespace capstan { namespace wire {
using value=dynabridge::rpc::value;
namespace codec=dynabridge::rpc::detail;
constexpr std::size_t max_frame=65536;
inline void require(bool v) { if(!v) CAPSTAN_RAISE(dynabridge::rpc::protocol_error("invalid Capstan wire value")); }
inline id number(const value& v) { require(v.kind()==dynabridge::rpc::value_kind::unsigned_integer); return v.as_unsigned(); }
inline std::string string(const value& v) { require(v.kind()==dynabridge::rpc::value_kind::string); return v.copy_string(); }
inline bool boolean(const value& v) { require(v.kind()==dynabridge::rpc::value_kind::boolean); return v.as_boolean(); }
inline value u(id n) { return value::unsigned_integer(n); }
inline std::string binary(const bytes& b) { return std::string(b.begin(),b.end()); }
inline bytes binary(const std::string& s) { return bytes(s.begin(),s.end()); }
inline bool is_number(const value& v) { return v.kind()==dynabridge::rpc::value_kind::unsigned_integer; }
inline bool is_string(const value& v) { return v.kind()==dynabridge::rpc::value_kind::string; }
inline bool is_boolean(const value& v) { return v.kind()==dynabridge::rpc::value_kind::boolean; }
inline bool try_decode(const bytes& b,codec::request& out) {
    if(b.size()>max_frame) return false;
    codec::request staged;
    if(!codec::try_decode_request(b,staged) || staged.args.size()>512) return false;
    out=std::move(staged);return true;
}
inline codec::request decode(const bytes& b) {codec::request out;require(try_decode(b,out));return out;}
inline std::string encode_grant(const grant& g) {
    return binary(codec::encode_request(800,{u(g.key.cluster),u(g.key.incarnation),u(g.key.object),u(g.epoch),u(g.grant_id),u(g.holder),u(static_cast<unsigned>(g.mode)),value::string(std::string(g.secret.begin(),g.secret.end()))}));
}
inline bool try_decode_grant(const std::string& s,grant& out) {
    codec::request r;if(!try_decode(binary(s),r) || r.method!=800 || r.args.size()!=8) return false;
    auto& a=r.args;
    for(unsigned i=0;i<7;++i) if(!is_number(a[i])) return false;
    if(!is_string(a[7]) || a[7].string_size()!=16) return false;
    auto mode=number(a[6]);if(mode!=1 && mode!=2) return false;
    grant g;g.key={number(a[0]),number(a[1]),number(a[2])};g.epoch=number(a[3]);
    g.grant_id=number(a[4]);g.holder=number(a[5]);g.mode=static_cast<access_mode>(mode);
    auto secret=string(a[7]);std::copy(secret.begin(),secret.end(),g.secret.begin());out=g;return true;
}
inline grant decode_grant(const std::string& s) {grant out;require(try_decode_grant(s,out));return out;}
inline bytes encode_transition(const transition& t) {
    std::vector<value> a{u(t.key.cluster),u(t.key.incarnation),u(t.key.object),u(t.control_session),u(t.tx),u(t.expected_epoch),u(t.next_epoch),value::boolean(t.destroy),u(static_cast<unsigned>(t.origin)),u(t.issuer)};
    for(const auto& g:t.grants) a.push_back(value::string(encode_grant(g)));
    return codec::encode_request(1801,a); // Authority schema v2; legacy 801 is rejected.
}
inline bool try_decode_transition(const bytes& b,transition& out) {
    codec::request r;if(!try_decode(b,r) || r.method!=1801 || r.args.size()<10 || r.args.size()>74) return false;
    auto& a=r.args;for(unsigned i=0;i<7;++i) if(!is_number(a[i])) return false;
    if(!is_boolean(a[7]) || !is_number(a[8]) || !is_number(a[9]) || number(a[8])>1) return false;
    transition t;t.key={number(a[0]),number(a[1]),number(a[2])};t.control_session=number(a[3]);
    t.tx=number(a[4]);t.expected_epoch=number(a[5]);t.next_epoch=number(a[6]);t.destroy=boolean(a[7]);t.origin=static_cast<authority_origin>(number(a[8]));t.issuer=number(a[9]);
    for(std::size_t i=10;i<a.size();++i) {
        grant g;if(!is_string(a[i]) || !try_decode_grant(string(a[i]),g)) return false;
        t.grants.push_back(g);
    }
    out=std::move(t);return true;
}
inline transition decode_transition(const bytes& b) {transition out;require(try_decode_transition(b,out));return out;}
struct reply { outcome state=outcome::unknown; status code=status::invalid; std::vector<value> values; };
inline bytes encode_reply(outcome state,status code,const std::vector<value>& values={}) {
    std::vector<value> a{u(static_cast<unsigned>(state)),u(static_cast<unsigned>(code))}; a.insert(a.end(),values.begin(),values.end());
    return codec::encode_response(value::string(binary(codec::encode_request(900,a))));
}
inline bool try_decode_reply(const bytes& b,reply& out) {
    if(b.size()>max_frame) return false;
    value encoded;std::string error;
    if(codec::try_decode_response(b,encoded,error)!=codec::response_status::ok || !is_string(encoded)) return false;
    codec::request r;if(!try_decode(binary(string(encoded)),r) || r.method!=900 || r.args.size()<2) return false;
    if(!is_number(r.args[0]) || !is_number(r.args[1])) return false;
    auto state=number(r.args[0]),code=number(r.args[1]);
    if(state>static_cast<unsigned>(outcome::unknown) || code>static_cast<unsigned>(status::superseded)) return false;
    out={static_cast<outcome>(state),static_cast<status>(code),{r.args.begin()+2,r.args.end()}};return true;
}
inline reply decode_reply(const bytes& b) {reply out;require(try_decode_reply(b,out));return out;}
struct call { request header; std::vector<value> args; };
inline bytes encode_call(const request& r,const std::vector<value>& args={}) {
    std::vector<value> a{value::string(encode_grant(r.capability)),u(r.request_id)}; a.insert(a.end(),args.begin(),args.end());
    return codec::encode_request(static_cast<unsigned>(r.operation),a);
}
inline bool try_decode_call(const bytes& b,call& out) {
    codec::request r;if(!try_decode(b,r) || r.args.size()<2 || r.args.size()>5 || r.method>std::numeric_limits<unsigned>::max()) return false;
    grant g;if(!is_string(r.args[0]) || !try_decode_grant(string(r.args[0]),g) || !is_number(r.args[1])) return false;
    out={{g,number(r.args[1]),static_cast<method>(r.method)},{r.args.begin()+2,r.args.end()}};return true;
}
inline call decode_call(const bytes& b) {call out;require(try_decode_call(b,out));return out;}
inline std::string encode_transfer_ref(const transfer_ref& ref) {
    return binary(codec::encode_request(1802,{u(ref.key.cluster),u(ref.key.incarnation),u(ref.key.object),
        u(static_cast<unsigned>(ref.transaction.origin)),u(ref.transaction.issuer),u(ref.transaction.sequence),
        u(ref.from_holder),u(ref.to_holder),u(ref.source_epoch)}));
}
inline bool try_decode_transfer_ref(const std::string& bytes,transfer_ref& out) {
    codec::request r;if(!try_decode(binary(bytes),r) || r.method!=1802 || r.args.size()!=9) return false;
    for(const auto& v:r.args) if(!is_number(v)) return false;
    const auto& a=r.args;if(number(a[3])!=1 || !number(a[4]) || !number(a[5]) || !number(a[6]) || !number(a[7]) || number(a[4])!=number(a[6])) return false;
    out={{number(a[0]),number(a[1]),number(a[2])},{authority_origin::holder_transfer,number(a[4]),number(a[5])},number(a[6]),number(a[7]),number(a[8])};return true;
}
inline std::string encode_transfer_request(const transfer_request& request) {
    return binary(codec::encode_request(1803,{value::string(encode_transfer_ref(request.reference())),value::string(encode_grant(request.source))}));
}
inline bool try_decode_transfer_request(const std::string& bytes,transfer_request& out) {
    codec::request r;transfer_ref ref;grant source;
    if(!try_decode(binary(bytes),r) || r.method!=1803 || r.args.size()!=2 || !is_string(r.args[0]) || !is_string(r.args[1]) ||
       !try_decode_transfer_ref(string(r.args[0]),ref) || !try_decode_grant(string(r.args[1]),source)) return false;
    transfer_request staged{source,ref.transaction,ref.to_holder};if(!(staged.reference()==ref)) return false;
    out=staged;return true;
}
inline std::string encode_transfer_receipt(const transfer_receipt& r) {
    return binary(codec::encode_request(1804,{value::string(encode_transfer_ref(r.reference)),u(static_cast<unsigned>(r.observation)),
        u(static_cast<unsigned>(r.usability)),u(r.installed_epoch),u(static_cast<unsigned>(r.code))}));
}
inline bool try_decode_transfer_receipt(const std::string& bytes,transfer_receipt& out) {
    codec::request r;transfer_ref ref;
    if(!try_decode(binary(bytes),r) || r.method!=1804 || r.args.size()!=5 || !is_string(r.args[0]) || !try_decode_transfer_ref(string(r.args[0]),ref)) return false;
    for(unsigned i=1;i<5;++i) if(!is_number(r.args[i])) return false;
    if(number(r.args[1])>3 || number(r.args[2])>4 || number(r.args[4])>static_cast<unsigned>(status::superseded)) return false;
    out={ref,static_cast<transfer_observation>(number(r.args[1])),static_cast<transfer_usability>(number(r.args[2])),number(r.args[3]),static_cast<status>(number(r.args[4]))};return true;
}
inline std::string encode_authority_view(const authority_view& view) {
    std::vector<value> args{u(view.key.cluster),u(view.key.incarnation),u(view.key.object),u(view.epoch),u(view.gate)};
    for(const auto& g:view.grants) args.push_back(value::string(encode_grant(g)));
    return binary(codec::encode_request(1805,args));
}
inline bool try_decode_authority_view(const std::string& bytes,authority_view& out) {
    codec::request r;if(!try_decode(binary(bytes),r) || r.method!=1805 || r.args.size()<5 || r.args.size()>69) return false;
    for(unsigned i=0;i<5;++i) if(!is_number(r.args[i])) return false;
    if(number(r.args[4])>2) return false;
    authority_view staged{{number(r.args[0]),number(r.args[1]),number(r.args[2])},number(r.args[3]),static_cast<unsigned>(number(r.args[4])),{}};
    for(std::size_t i=5;i<r.args.size();++i) {grant g;if(!is_string(r.args[i]) || !try_decode_grant(string(r.args[i]),g)) return false;staged.grants.push_back(g);}
    if(validate_target(staged.key,staged.epoch,staged.gate==2,staged.grants)!=status::ok) return false;
    out=std::move(staged);return true;
}
// Control methods are a separate namespace from business method/effect IDs.
constexpr id hello=1000, info=1001, set_authority=1002, query_ack=1003, stats=1004;
constexpr id configure_grants=2000, fetch_grant=2001;
constexpr id transfer_exclusive=3100, query_transfer=3101, claim_transfer=3102, query_current_authority=3103, fetch_my_grant=3104;
inline bytes command(id m,const std::vector<value>& args={}) { return codec::encode_request(m,args); }
}}
