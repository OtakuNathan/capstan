#include <capstan/protocol/wire.hpp>
#include "tests/check.hpp"
#include <limits>
using namespace capstan;
namespace {
// Compatibility oracle deliberately uses the generic Portal decoder.
bool reference_decode(const std::string& s,grant& out) {
    wire::codec::request r;
    if(!wire::try_decode(wire::binary(s),r) || r.method!=800 || r.args.size()!=8) return false;
    const auto& a=r.args;
    for(unsigned i=0;i<7;++i) if(!wire::is_number(a[i])) return false;
    if(!wire::is_string(a[7]) || a[7].string_size()!=16) return false;
    const auto mode=wire::number(a[6]);if(mode!=1 && mode!=2) return false;
    grant staged;
    staged.key={wire::number(a[0]),wire::number(a[1]),wire::number(a[2])};
    staged.epoch=wire::number(a[3]);staged.grant_id=wire::number(a[4]);staged.holder=wire::number(a[5]);
    staged.mode=static_cast<access_mode>(mode);
    const auto secret=wire::string(a[7]);std::copy(secret.begin(),secret.end(),staged.secret.begin());
    out=staged;return true;
}
grant sample() {
    grant g;g.key={0,std::numeric_limits<id>::max(),0x0102030405060708ull};
    g.epoch=0x8000000000000000ull;g.grant_id=17;g.holder=23;
    for(unsigned i=0;i<g.secret.size();++i) g.secret[i]=static_cast<unsigned char>(i*17);
    return g;
}
void compare(const std::string& frame) {
    const auto sentinel=sample();auto expected=sentinel,actual=sentinel;
    const bool accepted=reference_decode(frame,expected);
    CHECK(wire::try_decode_grant(frame,actual)==accepted);
    CHECK(actual==expected);
    if(!accepted) CHECK(actual==sentinel);
}
void compatibility_and_ownership() {
    std::uint64_t rng=0x5eed;
    auto next=[&] {rng=rng*6364136223846793005ull+1;return rng;};
    for(unsigned round=0;round<1024;++round) {
        auto g=sample();
        if(round>1) {
            g.key={next(),next(),next()};g.epoch=next();g.grant_id=next();g.holder=next();
            for(auto& c:g.secret) c=static_cast<unsigned char>(next()>>32);
        }
        g.mode=round%2 ? access_mode::exclusive : access_mode::shared;
        auto frame=wire::encode_grant(g);compare(frame);
        grant decoded;CHECK(wire::try_decode_grant(frame,decoded));CHECK(decoded==g);
        std::fill(frame.begin(),frame.end(),0);frame.clear();frame.shrink_to_fit();
        CHECK(decoded==g); // No borrowed token bytes survive decoding.
    }
    // Structural decoding must not start enforcing authority policy.
    auto zero=grant{};zero.mode=access_mode::exclusive;
    grant decoded;CHECK(wire::try_decode_grant(wire::encode_grant(zero),decoded));CHECK(decoded==zero);
}
void malformed() {
    const auto valid=wire::encode_grant(sample());
    for(std::size_t n=0;n<valid.size();++n) compare(valid.substr(0,n));
    // Every one-byte substitution, including high bytes in tags, lengths and mode.
    for(std::size_t i=0;i<valid.size();++i) {
        for(unsigned n=0;n<256;++n) {auto frame=valid;frame[i]=static_cast<char>(n);compare(frame);}
    }
    compare(valid+std::string(1,'\0'));
    compare(std::string(wire::max_frame+1,'\0'));
    auto parsed=wire::decode(wire::binary(valid));
    for(std::size_t n:{std::size_t(0),std::size_t(15),std::size_t(17),std::size_t(64)}) {
        auto a=parsed.args;a[7]=wire::value::string(std::string(n,'x'));
        compare(wire::binary(wire::codec::encode_request(800,a)));
    }
    auto a=parsed.args;a.pop_back();compare(wire::binary(wire::codec::encode_request(800,a)));
    a=parsed.args;a.push_back(wire::u(0));compare(wire::binary(wire::codec::encode_request(800,a)));
    std::uint32_t rng=0x12345;
    for(unsigned round=0;round<2048;++round) {
        std::string frame(round%192,'\0');
        for(auto& c:frame) {rng=rng*1664525u+1013904223u;c=static_cast<char>(rng>>24);}
        compare(frame);
    }
}
}
int main() {compatibility_and_ownership();malformed();}
