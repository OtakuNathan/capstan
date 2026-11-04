#include <capstan/access/capability.hpp>
#include <capstan/authority/host.hpp>
#include "tests/check.hpp"
using namespace capstan;
int main() {
    dynabridge::rpc::router router;
    dynabridge::rpc_backend::export_context_t context;
    int calls=0;
    router.define("increment",dynabridge::create_export_callable_binder<int(int)>(context,
        [&](int x) noexcept {++calls;return x+1;}));
    auto method_id=wire::codec::symbol_id("increment");
    for(const auto& args:std::vector<std::vector<wire::value>>{{},{wire::value::boolean(true)},{wire::u(1),wire::u(2)}}) {
        auto response=router.dispatch(wire::command(method_id,args));wire::value out;std::string message;
        CHECK(wire::codec::try_decode_response(response,out,message)==wire::codec::response_status::remote_error);
    }
    CHECK(calls==0);
    auto response=router.dispatch(wire::command(method_id,{wire::value::signed_integer(7)}));
    wire::value out;std::string message;
    CHECK(wire::codec::try_decode_response(response,out,message)==wire::codec::response_status::ok);
    CHECK(out.as_signed()==8 && calls==1);
    auto valid=wire::command(wire::info,{wire::u(1),wire::value::string("text")});
    for(std::size_t n=0;n<valid.size();++n) {
        wire::codec::request result;result.method=77;
        CHECK(!wire::try_decode(bytes(valid.begin(),valid.begin()+n),result));
        CHECK(result.method==77); // No partial publication on malformed input.
    }
    auto bad=valid;bad.push_back(0);wire::codec::request parsed;
    CHECK(!wire::try_decode(bad,parsed));
    bad=valid;bad[17]=255;CHECK(!wire::try_decode(bad,parsed));
    wire::value result;std::string error;
    auto remote=wire::codec::encode_error("remote business failure");
    CHECK(wire::codec::try_decode_response(remote,result,error)==wire::codec::response_status::remote_error);
    CHECK(error=="remote business failure");remote.push_back(0);
    CHECK(wire::codec::try_decode_response(remote,result,error)==wire::codec::response_status::malformed);
    grant g;g.key={1,2,3};g.epoch=1;g.grant_id=1;g.holder=1;g.secret[0]=1;g.mode=access_mode::exclusive;
    dataset_binding dataset(g.key,1024,7);
    CHECK(!dataset.try_dispatch(method::add_range,g,{wire::value::boolean(true),wire::u(1),wire::u(2)},result));
    CHECK(!dataset.try_dispatch(method::add_range,g,{wire::u(10),wire::u(1),wire::u(2)},result));
    CHECK(!dataset.try_dispatch(method::add_range,g,{wire::u(0),wire::u(129),wire::u(2)},result));
    CHECK(dataset.try_dispatch(method::version,g,{},result) && wire::number(result)==0);
    exclusive_capability cap(g);
    status code;
    CHECK(!exclusive_capability::try_create(g,code) && code==status::duplicate);
    {auto held=cap.try_add_range(0,1,1);CHECK(held);CHECK(!cap.try_add_range(0,1,1));CHECK(!cap.try_version());}
    CHECK(cap.try_version());
    // Contract failure remains fail-stop under NDEBUG; ordinary errors above
    // remain values, so the process continues and the resource is unchanged.
    CHECK(rejects<dynabridge::rpc::protocol_error>([]{wire::require(false);}));
}
