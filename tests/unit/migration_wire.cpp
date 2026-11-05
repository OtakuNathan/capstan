#include <capstan/migration/wire.hpp>
#include "tests/check.hpp"
namespace {
using namespace capstan;using namespace capstan::migration;
bytes chunk_frame(std::uint64_t offset,const std::uint8_t* data,std::size_t size) {
    return wire::encode_reply(outcome::completed,status::ok,
        {wire::u(offset),wire::value::string(std::string(reinterpret_cast<const char*>(data),size))});
}
void compare_chunk(const bytes& frame) {
    wire::reply decoded;
    const bool expected=wire::try_decode_reply(frame,decoded) && decoded.code==status::ok &&
        decoded.values.size()==2 && wire::is_number(decoded.values[0]) && wire::is_string(decoded.values[1]);
    const std::uint8_t sentinel=42;
    chunk_reply_view view{71,&sentinel,19};
    CHECK(try_decode_chunk_reply(frame,view)==expected);
    if(expected) {
        const auto& payload=decoded.values[1];
        CHECK(view.offset==wire::number(decoded.values[0]) && view.size==payload.string_size());
        CHECK(view.data==frame.data()+frame.size()-view.size);
        CHECK(std::equal(view.data,view.data+view.size,reinterpret_cast<const std::uint8_t*>(payload.string_data())));
    } else CHECK(view.offset==71 && view.data==&sentinel && view.size==19);
}
void chunks() {
    for(std::size_t size:{std::size_t(0),std::size_t(1),std::size_t(23),transfer_receiver::chunk_bytes}) {
        buffer source(size+1,0xa5);source[0]=0;
        auto frame=chunk_frame(123,source.data(),size);
        const auto expected=frame;
        // The outgoing frame owns its bytes, independently of source retention.
        std::fill(source.begin(),source.end(),0);source.clear();source.shrink_to_fit();
        CHECK(frame==expected);compare_chunk(frame);
        if(size==23) {
            for(std::size_t n=0;n<frame.size();++n) compare_chunk(bytes(frame.begin(),frame.begin()+n));
            for(std::size_t i=0;i<frame.size();++i) for(unsigned bit=0;bit<8;++bit) {
                auto bad=frame;bad[i]^=1u<<bit;compare_chunk(bad);
            }
        }
    }
    for(unsigned s=0;s<=static_cast<unsigned>(outcome::unknown);++s)
        for(unsigned c=0;c<=static_cast<unsigned>(status::superseded);++c)
            compare_chunk(wire::encode_reply(static_cast<outcome>(s),static_cast<status>(c),{wire::u(0),wire::value::string("data")}));
    compare_chunk(wire::encode_reply(outcome::completed,status::ok,{wire::u(0)}));
    compare_chunk(wire::encode_reply(outcome::completed,status::ok,{wire::u(0),wire::u(0)}));
    compare_chunk(wire::encode_reply(outcome::completed,status::ok,{wire::u(0),wire::value::string("data"),wire::u(0)}));
    compare_chunk(wire::encode_reply(outcome::completed,status::ok,{wire::u(0),wire::value::string(std::string(wire::max_frame,'x'))}));
    compare_chunk(wire::codec::encode_error("remote failure"));

    manifest m;m.transaction={1,2};m.object={3,4,5};m.source_host=6;m.target_host=7;
    m.source_instance=8;m.target_instance=9;m.generation=10;m.size=2*transfer_receiver::chunk_bytes+1;
    buffer original(m.size);for(std::size_t i=0;i<original.size();++i) original[i]=static_cast<std::uint8_t>(i);
    transfer_receiver receiver;CHECK(receiver.begin(m)==status::ok);
    for(std::size_t offset:{transfer_receiver::chunk_bytes,std::size_t(0),2*transfer_receiver::chunk_bytes}) {
        const auto size=std::min(std::size_t(transfer_receiver::chunk_bytes),original.size()-offset);
        auto frame=chunk_frame(offset,original.data()+offset,size);
        chunk_reply_view view;CHECK(try_decode_chunk_reply(frame,view));
        CHECK(receiver.append(m,view.offset,view.data,view.size)==status::ok);
        CHECK(receiver.append(m,view.offset,view.data,view.size)==status::ok); // duplicate
        frame.back()^=1;
        CHECK(receiver.append(m,view.offset,view.data,view.size)==status::conflict);
        std::fill(frame.begin(),frame.end(),0); // receiver must already own its copy
    }
    CHECK(receiver.seal(m,[](const buffer&,const std::array<std::uint8_t,32>&) noexcept {return status::ok;})==status::ok);
    CHECK(*receiver.prepared_data()==original);
    auto late=chunk_frame(0,original.data(),transfer_receiver::chunk_bytes);
    chunk_reply_view view;CHECK(try_decode_chunk_reply(late,view));
    CHECK(receiver.abort(m)==status::ok);
    CHECK(receiver.append(m,view.offset,view.data,view.size)==status::closed);
}
}
int main() {
    chunks();
    using namespace capstan;using namespace capstan::migration;
    manifest m;m.transaction={1,2};m.object={3,4,5};m.source_host=6;m.target_host=7;
    m.source_instance=8;m.target_instance=9;m.generation=10;m.size=11;m.digest[0]=255;
    auto encoded=encode_manifest(m);manifest decoded;
    CHECK(decode_manifest(encoded,decoded) && decoded==m);
    for(std::size_t size=0;size<encoded.size();++size) {
        CHECK(!decode_manifest(encoded.substr(0,size),decoded));CHECK(decoded==m);
    }
    auto parsed=wire::decode(wire::binary(encoded));
    for(unsigned i=0;i<11;++i) {
        auto args=parsed.args;args[i]=wire::value::boolean(true);
        CHECK(!decode_manifest(wire::binary(wire::command(1810,args)),decoded));CHECK(decoded==m);
    }
    auto args=parsed.args;args[11]=wire::value::string(std::string(31,'a'));
    CHECK(!decode_manifest(wire::binary(wire::command(1810,args)),decoded));
    m.transaction.control=0;CHECK(!decode_manifest(encode_manifest(m),decoded));
}
