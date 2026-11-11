#include <capstan/protocol/wire.hpp>
#include "tests/check.hpp"
#include <cstring>
using namespace capstan;
namespace {
// Independent compatibility oracle: compose the original Portal codecs.
bytes reference_encode(outcome state,status code,const std::vector<wire::value>& values) {
    std::vector<wire::value> args{wire::u(static_cast<unsigned>(state)),wire::u(static_cast<unsigned>(code))};
    args.insert(args.end(),values.begin(),values.end());
    return wire::codec::encode_response(wire::value::string(wire::binary(wire::codec::encode_request(900,args))));
}
bool reference_decode(const bytes& frame,wire::reply& out) {
    if(frame.size()>wire::max_frame) return false;
    wire::value encoded;std::string error;
    if(wire::codec::try_decode_response(frame,encoded,error)!=wire::codec::response_status::ok || !wire::is_string(encoded)) return false;
    wire::codec::request r;
    if(!wire::try_decode(wire::binary(wire::string(encoded)),r) || r.method!=900 || r.args.size()<2) return false;
    if(!wire::is_number(r.args[0]) || !wire::is_number(r.args[1])) return false;
    const auto state=wire::number(r.args[0]),code=wire::number(r.args[1]);
    if(state>static_cast<unsigned>(outcome::unknown) || code>static_cast<unsigned>(status::superseded)) return false;
    out={static_cast<outcome>(state),static_cast<status>(code),{r.args.begin()+2,r.args.end()}};return true;
}
bool equivalent(const wire::reply& a,const wire::reply& b) {
    return reference_encode(a.state,a.code,a.values)==reference_encode(b.state,b.code,b.values);
}
void compare(const bytes& frame) {
    const wire::reply sentinel{outcome::unknown,status::pending,{wire::value::string("unchanged")}};
    auto expected=sentinel,actual=sentinel;
    const bool accepted=reference_decode(frame,expected);
    CHECK(wire::try_decode_reply(frame,actual)==accepted);
    CHECK(equivalent(actual,expected));
    if(!accepted) CHECK(equivalent(actual,sentinel));
}
void compatibility_and_ownership() {
    std::string borrowed(48*1024,'x');borrowed[123]='\0';borrowed.back()=static_cast<char>(255);
    const std::vector<wire::value> values{wire::value{},wire::value::signed_integer(-17),
        wire::u(std::numeric_limits<id>::max()),wire::value::real(-0.0),wire::value::real(3.125),
        wire::value::boolean(false),wire::value::boolean(true),wire::value::string(""),
        wire::value::string(std::string("a\0b",3)),wire::value::borrowed_string(borrowed)};
    for(unsigned s=0;s<=static_cast<unsigned>(outcome::unknown);++s) {
        for(unsigned c=0;c<=static_cast<unsigned>(status::superseded);++c) {
            const auto state=static_cast<outcome>(s);const auto code=static_cast<status>(c);
            auto expected=reference_encode(state,code,values);
            auto frame=wire::encode_reply(state,code,values);
            CHECK(frame==expected);compare(frame);
            wire::reply decoded;CHECK(wire::try_decode_reply(frame,decoded));
            // Destroy every input byte. Output and its copy must remain owning.
            std::fill(frame.begin(),frame.end(),0);frame.clear();frame.shrink_to_fit();
            auto copied=decoded;auto moved=std::move(decoded);
            CHECK(reference_encode(copied.state,copied.code,copied.values)==expected);
            CHECK(reference_encode(moved.state,moved.code,moved.values)==expected);
        }
    }
    CHECK(wire::encode_reply(outcome::completed,status::ok)==reference_encode(outcome::completed,status::ok,{}));
}
void malformed_and_limits() {
    const auto valid=reference_encode(outcome::completed,status::ok,
        {wire::u(19),wire::value::string(std::string("binary\0payload",14)),wire::value::boolean(true)});
    for(std::size_t n=0;n<valid.size();++n) compare(bytes(valid.begin(),valid.begin()+n));
    // Differential mutations cover both envelopes, length/count fields and values.
    for(std::size_t i=0;i<valid.size();++i) {
        for(unsigned bit=0;bit<8;++bit) {auto b=valid;b[i]^=1u<<bit;compare(b);}
    }
    auto b=valid;b.push_back(0);compare(b);
    for(std::size_t offset:{std::size_t(7),std::size_t(24)}) {
        for(std::uint32_t n:{0u,1u,2u,512u,513u,65535u,0xffffffffu}) {
            b=valid;for(unsigned i=0;i<4;++i) b[offset+i]=static_cast<std::uint8_t>(n>>(i*8));compare(b);
        }
    }
    compare(wire::codec::encode_error("remote failure"));
    compare(wire::codec::encode_response(wire::u(0)));
    for(unsigned n:{510u,511u}) {
        b=reference_encode(outcome::completed,status::ok,std::vector<wire::value>(n));
        wire::reply out;CHECK(wire::try_decode_reply(b,out)==(n==510));compare(b);
    }
    for(std::size_t extra:{std::size_t(0),std::size_t(1)}) {
        b=reference_encode(outcome::completed,status::ok,{wire::value::string(std::string(wire::max_frame-51+extra,'z'))});
        CHECK(b.size()==wire::max_frame+extra);
        wire::reply out;CHECK(wire::try_decode_reply(b,out)==(extra==0));compare(b);
    }
    // Reproducible arbitrary frames; malformed input never changes the output.
    std::uint32_t rng=0x5eed;
    for(unsigned round=0;round<1024;++round) {
        b.resize(round%128);
        for(auto& byte:b) {rng=rng*1664525u+1013904223u;byte=static_cast<std::uint8_t>(rng>>24);}
        compare(b);
    }
}
}
int main() {compatibility_and_ownership();malformed_and_limits();}
