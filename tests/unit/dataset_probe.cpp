#include <capstan/bridge/dataset.hpp>
#include "tests/check.hpp"
#include <limits>
#include <new>

static std::size_t allocations=0;
void* operator new(std::size_t size) {
    ++allocations;
    if(auto* p=std::malloc(size ? size : 1)) return p;
    std::abort();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}

using namespace capstan;
using value=wire::value;
using summary_converter=dataset_backend::converter<summary_t>;

int main() {
    dataset_backend::context ctx;
    const summary_t expected{0,std::numeric_limits<std::uint64_t>::max(),42,7};
    auto encoded=summary_converter::to(ctx,expected);
    const auto text=encoded.copy_string();
    auto borrowed=value::borrowed_string(text);
    for(const auto* input:{&encoded,&borrowed}) {
        const auto before=allocations;
        CHECK(summary_converter::probe(ctx,*input));
        CHECK(summary_converter::probe(ctx,*input));
        auto result=dynabridge::from_optional<summary_t>(ctx,*input);
        CHECK(result && *result==expected);
        CHECK(allocations==before);
        CHECK(input->copy_string()==text);
    }
    int calls=0;
    auto binder=dynabridge::create_export_callable_binder<std::uint64_t(summary_t)>(ctx,
        [&](summary_t input) noexcept {++calls;return input.version;});
    CHECK(binder(encoded).as_unsigned()==expected.version);
    CHECK(binder(borrowed).as_unsigned()==expected.version);
    CHECK(calls==2 && encoded.copy_string()==text && borrowed.copy_string()==text);
    CHECK(rejects<dynabridge::bad_conversion>([&]{binder(value::string("malformed"));}));
    CHECK(calls==2);
    auto reject=[&](const value& input) {
        const auto before=allocations;
        CHECK(!summary_converter::probe(ctx,input));
        CHECK(!dynabridge::from_optional<summary_t>(ctx,input));
        CHECK(allocations==before);
    };
    reject(value::unsigned_integer(1));
    for(std::size_t n=0;n<text.size();++n) reject(value::string(text.substr(0,n)));
    reject(value::string(text+"x"));
    for(std::size_t offset:{0u,4u,5u,13u,17u,26u,35u,44u}) {
        auto bad=text;bad[offset]=static_cast<char>(255);reject(value::string(bad));
    }
    for(auto kind:{dynabridge::rpc::value_kind::signed_integer,dynabridge::rpc::value_kind::real,
                   dynabridge::rpc::value_kind::boolean,dynabridge::rpc::value_kind::string,
                   dynabridge::rpc::value_kind::undefined}) {
        auto bad=text;bad[17]=static_cast<char>(kind);reject(value::string(bad));
    }
    // Differential check against the original general codec: preserve both
    // accepted payload bits and rejection of every single-byte mutation.
    for(std::size_t offset=0;offset<text.size();++offset) for(unsigned byte=0;byte<256;++byte) {
        auto changed=text;changed[offset]=static_cast<char>(byte);
        auto input=value::borrowed_string(changed);
        wire::codec::request decoded;
        bool accepted=wire::codec::try_decode_request(bytes(changed.begin(),changed.end()),decoded);
        accepted=accepted && decoded.method==901 && decoded.args.size()==4;
        if(accepted) for(const auto& arg:decoded.args) accepted=accepted && wire::is_number(arg);
        CHECK(summary_converter::probe(ctx,input)==accepted);
        auto result=dynabridge::from_optional<summary_t>(ctx,input);
        CHECK(bool(result)==accepted);
        if(accepted) {
            const summary_t old{decoded.args[0].as_unsigned(),decoded.args[1].as_unsigned(),
                                decoded.args[2].as_unsigned(),decoded.args[3].as_unsigned()};
            CHECK(*result==old);
        }
    }
    CHECK(dataset_backend::converter<std::uint64_t>::probe(ctx,value::unsigned_integer(UINT64_MAX)));
    CHECK(!dataset_backend::converter<std::uint64_t>::probe(ctx,value::signed_integer(1)));
    CHECK(!dataset_backend::converter<std::uint64_t>::probe(ctx,value::boolean(true)));

    grant g;g.key={1,2,3};g.epoch=1;g.grant_id=1;g.holder=1;g.secret[0]=1;g.mode=access_mode::exclusive;
    dataset_binding dataset(g.key,1024,7);
    value result;
    CHECK(dataset.try_dispatch(method::summary,g,{},result));
    wire::reply reply;reply.state=outcome::completed;reply.code=status::ok;reply.values={result};
    summary_t initial;
    CHECK(typed_call<summary_t>::try_decode(reply,initial));
    const std::vector<value> args{wire::u(0),wire::u(1),wire::u(2)};
    CHECK(dataset.try_dispatch(method::add_range,g,args,result));
    CHECK(result.as_unsigned()==1);
    CHECK(args[0].as_unsigned()==0 && args[1].as_unsigned()==1 && args[2].as_unsigned()==2);
    CHECK(dataset.try_dispatch(method::summary,g,{},result));
    reply.values={result};summary_t updated;
    CHECK(typed_call<summary_t>::try_decode(reply,updated));
    CHECK(updated.version==1 && updated.sum==initial.sum+2 && updated.count==initial.count);
    reply.values={value::string("malformed")};
    CHECK(!typed_call<summary_t>::try_decode(reply,updated));
    CHECK(updated.version==1);
    CHECK(!dataset.try_dispatch(method::add_range,g,{value::signed_integer(0),wire::u(1),wire::u(2)},result));
}
