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
    std::vector<value> a{u(t.key.cluster),u(t.key.incarnation),u(t.key.object),u(t.control_session),u(t.tx),u(t.expected_epoch),u(t.next_epoch),value::boolean(t.destroy)};
    for(const auto& g:t.grants) a.push_back(value::string(encode_grant(g)));
    return codec::encode_request(801,a);
}
inline bool try_decode_transition(const bytes& b,transition& out) {
    codec::request r;if(!try_decode(b,r) || r.method!=801 || r.args.size()<8 || r.args.size()>72) return false;
    auto& a=r.args;for(unsigned i=0;i<7;++i) if(!is_number(a[i])) return false;
    if(!is_boolean(a[7])) return false;
    transition t;t.key={number(a[0]),number(a[1]),number(a[2])};t.control_session=number(a[3]);
    t.tx=number(a[4]);t.expected_epoch=number(a[5]);t.next_epoch=number(a[6]);t.destroy=boolean(a[7]);
    for(std::size_t i=8;i<a.size();++i) {
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
    if(state>static_cast<unsigned>(outcome::unknown) || code>static_cast<unsigned>(status::invalid)) return false;
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
// Control methods are a separate namespace from business method/effect IDs.
constexpr id hello=1000, info=1001, set_authority=1002, query_ack=1003, stats=1004;
constexpr id configure_grants=2000, fetch_grant=2001;
inline bytes command(id m,const std::vector<value>& args={}) { return codec::encode_request(m,args); }
}}
