#include <capstan/protocol/wire.hpp>
#include <capstan/authority/host.hpp>
#include "tests/check.hpp"
#include <limits>
using namespace capstan;
namespace {
template<class F> void malformed(F f) {
    bool rejected=rejects<dynabridge::rpc::protocol_error>(f);
    CHECK(rejected);
}
grant sample() {
    grant g;g.key={2,3,5};g.epoch=1;g.grant_id=7;g.holder=11;g.secret[0]=13;return g;
}
void check_frames() {
    auto frame=wire::command(wire::info);
    CHECK(wire::decode(frame).method==wire::info);
    for(std::size_t length=0;length<frame.size();++length) {
        auto partial=bytes(frame.begin(),frame.begin()+length);
        malformed([&]{wire::decode(partial);});
    }
    auto bad=frame;bad[4]=255;malformed([&]{wire::decode(bad);}); // codec version
    bad=frame;bad[0]^=0xff;malformed([&]{wire::decode(bad);});
    bad=frame;bad.push_back(0);malformed([&]{wire::decode(bad);});
    bad.resize(wire::max_frame+1);malformed([&]{wire::decode(bad);});
    std::vector<wire::value> args(513,wire::u(0));
    bad=wire::command(wire::info,args);malformed([&]{wire::decode(bad);});
    malformed([&]{wire::number(wire::value::signed_integer(1));});
    malformed([&]{wire::number(wire::value::real(1));});
    malformed([&]{wire::string(wire::u(1));});
    malformed([&]{wire::boolean(wire::u(1));});
}
void check_nested_grants_and_calls() {
    auto g=sample();
    CHECK(wire::decode_grant(wire::encode_grant(g))==g);
    auto parsed=wire::decode(wire::binary(wire::encode_grant(g)));
    auto malformed_grant=[&](std::vector<wire::value> a,id tag=800) {
        malformed([&]{wire::decode_grant(wire::binary(wire::codec::encode_request(tag,a)));});
    };
    auto a=parsed.args;a.pop_back();malformed_grant(a);
    a=parsed.args;a.push_back(wire::u(1));malformed_grant(a);
    malformed_grant(parsed.args,801);
    for(unsigned i=0;i<7;++i) {
        a=parsed.args;a[i]=wire::value::boolean(true);malformed_grant(a);
    }
    a=parsed.args;a[6]=wire::u(0);malformed_grant(a);
    a=parsed.args;a[6]=wire::u(3);malformed_grant(a);
    a=parsed.args;a[7]=wire::value::string(std::string(15,'x'));malformed_grant(a);
    a=parsed.args;a[7]=wire::value::string(std::string(17,'x'));malformed_grant(a);
    a=parsed.args;a[7]=wire::u(0);malformed_grant(a);
    auto malformed_call=[&](id m,std::vector<wire::value> args) {
        malformed([&]{wire::decode_call(wire::command(m,args));});
    };
    malformed_call(1,{});
    malformed_call(1,{wire::value::string("garbage"),wire::u(1)});
    malformed_call(1,{wire::value::string(wire::encode_grant(g)),wire::value::signed_integer(1)});
    malformed_call(std::numeric_limits<id>::max(),{wire::value::string(wire::encode_grant(g)),wire::u(1)});
    auto call=wire::decode_call(wire::encode_call({g,1,static_cast<method>(99)}));
    host_authority host(g.key,17);
    CHECK(host.set_authority({g.key,17,1,0,1,false,{g}})==status::ok);
    CHECK(host.admit(call.header,g.holder).code==status::unknown_method);
    // Framing parses a typed scalar; strict argument conversion is a separate
    // Dynabridge binder layer (covered by probe_object and loopback tests).
    auto typed=wire::decode_call(wire::encode_call({g,2,method::add_range},{wire::value::boolean(true)}));
    CHECK(typed.args[0].kind()==dynabridge::rpc::value_kind::boolean);
}
void check_transitions_and_replies() {
    auto g=sample();transition t{g.key,17,1,0,1,false,{g}};
    CHECK(wire::decode_transition(wire::encode_transition(t))==t);
    auto parsed=wire::decode(wire::encode_transition(t));
    auto a=parsed.args;a[7]=wire::u(0);
    malformed([&]{wire::decode_transition(wire::command(801,a));});
    a=parsed.args;a[8]=wire::value::string("invalid grant");
    malformed([&]{wire::decode_transition(wire::command(801,a));});
    a=parsed.args;a.resize(7);
    malformed([&]{wire::decode_transition(wire::command(801,a));});
    a=parsed.args;a.resize(73);
    malformed([&]{wire::decode_transition(wire::command(801,a));});
    t.grants.push_back(g);
    auto decoded=wire::decode_transition(wire::encode_transition(t));
    host_authority host(g.key,17);
    CHECK(host.set_authority(decoded)==status::invalid); // Structural parse != authorization.
    t.grants.resize(1);t.destroy=true;
    CHECK(host.set_authority(wire::decode_transition(wire::encode_transition(t)))==status::invalid);
    for(auto state : {outcome::not_submitted,outcome::rejected,outcome::completed,outcome::unknown}) {
        auto r=wire::decode_reply(wire::encode_reply(state,status::ok,{wire::u(23)}));
        CHECK(r.state==state && r.code==status::ok && r.values.size()==1 && wire::number(r.values[0])==23);
    }
    auto malformed_reply=[&](id tag,std::vector<wire::value> args) {
        auto encoded=wire::codec::encode_response(wire::value::string(wire::binary(wire::command(tag,args))));
        malformed([&]{wire::decode_reply(encoded);});
    };
    malformed_reply(900,{});
    malformed_reply(901,{wire::u(0),wire::u(0)});
    malformed_reply(900,{wire::u(4),wire::u(0)});
    malformed_reply(900,{wire::u(0),wire::u(static_cast<id>(status::invalid)+1)});
    malformed_reply(900,{wire::value::boolean(false),wire::u(0)});
    auto response=wire::encode_reply(outcome::completed,status::ok);response.push_back(0);
    malformed([&]{wire::decode_reply(response);});
}
}
int main() {
    check_frames();check_nested_grants_and_calls();check_transitions_and_replies();
    std::cout << "wire: truncated/version/length/tag/type/mode/nested errors, unknown method admission, invalid targets and outcome categories passed\n";
}
