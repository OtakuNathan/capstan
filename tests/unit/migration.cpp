#include <capstan/migration/decision.hpp>
#include "tests/check.hpp"
#include "examples/dataset/dataset.hpp"
using namespace capstan;
namespace m=capstan::migration;
int main() {
    using dataset=dynabridge::capstan_native::dataset;
    dataset source(1024,17);source.add_range(0,16,42);
    m::buffer captured;CHECK(m::snapshot_traits<dataset>::capture(source,captured)==status::ok);
    auto summary=source.summary();source.add_range(0,16,1);
    std::unique_ptr<dataset> copy1,copy2;
    CHECK(m::snapshot_traits<dataset>::restore(captured,copy1)==status::ok);
    CHECK(m::snapshot_traits<dataset>::restore(captured,copy2)==status::ok);
    CHECK(copy1->summary().sum==summary.sum && copy1->version()==summary.version);
    copy1->add_range(0,16,2);
    CHECK(copy2->summary().sum==summary.sum && copy2->version()==summary.version);
    CHECK(source.summary().sum==summary.sum+16);
    m::dataset_snapshot original;original.version=27;original.words={0,1,0x0123456789abcdefULL,~std::uint64_t(0)};
    m::buffer encoded;CHECK(m::encode(original,encoded)==status::ok);
    CHECK(encoded.size()==92 && encoded[0]=='C' && encoded[3]=='N');
    CHECK(encoded[76]==0xef && encoded[83]==0x01);
    m::buffer prefixed{9,8,7};const auto prefix=prefixed;
    CHECK(m::append_snapshot(original.version,original.words,prefixed)==status::ok);
    CHECK(std::equal(prefix.begin(),prefix.end(),prefixed.begin()));
    CHECK(m::buffer(prefixed.begin()+prefix.size(),prefixed.end())==encoded);
    const auto intact=prefixed;
    CHECK(m::append_snapshot(0,{},prefixed)==status::invalid && prefixed==intact);
    CHECK(m::encode(0,{},prefixed)==status::invalid && prefixed==intact);
    m::dataset_snapshot decoded;CHECK(m::decode(encoded,decoded)==status::ok);
    CHECK(decoded.version==original.version && decoded.words==original.words);
    for(std::size_t i=0;i<encoded.size();++i) {
        m::buffer truncated(encoded.begin(),encoded.begin()+i);
        CHECK(m::decode(truncated,decoded)==status::invalid);
        CHECK(decoded.words==original.words);
    }
    auto optional=encoded;m::put(optional,19,4);m::put(optional,3,8);optional.insert(optional.end(),{1,2,3});
    CHECK(m::decode(optional,decoded)==status::ok);
    optional[encoded.size()+3]=0x80;CHECK(m::decode(optional,decoded)==status::invalid);
    auto duplicate=encoded;duplicate.insert(duplicate.end(),encoded.begin()+8,encoded.begin()+28);
    CHECK(m::decode(duplicate,decoded)==status::invalid);
    m::manifest identity;identity.transaction={7,8};identity.object={1,2,3};identity.source_host=1;identity.target_host=2;
    identity.source_instance=11;identity.target_instance=22;identity.generation=1;
    identity.size=m::transfer_receiver::chunk_bytes+7;
    m::transfer_receiver receiver;
    CHECK(receiver.begin(identity)==status::ok);CHECK(receiver.begin(identity)==status::ok);
    auto different=identity;++different.target_instance;CHECK(receiver.begin(different)==status::conflict);
    m::buffer first(m::transfer_receiver::chunk_bytes,42),last(7,17);
    auto validate=[](const m::buffer&,const std::array<std::uint8_t,32>&) noexcept {return status::ok;};
    CHECK(receiver.seal(identity,validate)==status::pending);
    CHECK(receiver.append(identity,first.size(),last)==status::ok);
    CHECK(receiver.received()==last.size() && receiver.contiguous_bytes()==0);
    CHECK(receiver.seal(identity,validate)==status::pending);
    CHECK(receiver.append(identity,0,first)==status::ok);CHECK(receiver.append(identity,0,first)==status::ok);
    first[0]=41;CHECK(receiver.append(identity,0,first)==status::conflict);
    CHECK(receiver.append(identity,first.size(),last)==status::ok);
    CHECK(receiver.seal(identity,[](const m::buffer&,const std::array<std::uint8_t,32>&) noexcept {return status::invalid;})==status::invalid);
    CHECK(!receiver.prepared_data());CHECK(receiver.seal(identity,validate)==status::ok);
    auto retained=receiver.prepared_data();CHECK(retained && retained->size()==identity.size);
    CHECK(receiver.abort(identity)==status::ok);CHECK(retained->size()==identity.size);
    CHECK(receiver.begin(identity)==status::closed);CHECK(receiver.append(identity,0,first)==status::closed);
    m::transfer_receiver early;CHECK(early.abort(identity)==status::ok);CHECK(early.begin(identity)==status::closed);
    m::transfer_receiver limited(8);CHECK(limited.begin(identity)==status::exhausted);
    m::transfer_receiver reentrant;
    CHECK(reentrant.begin(identity)==status::ok);first[0]=42;
    CHECK(reentrant.append(identity,0,first)==status::ok);
    CHECK(reentrant.append(identity,first.size(),last)==status::ok);
    CHECK(reentrant.seal(identity,[&](const m::buffer& held,const std::array<std::uint8_t,32>&) noexcept {
        CHECK(reentrant.abort(identity)==status::ok);
        CHECK(held.size()==identity.size && held.front()==42);
        return status::ok;
    })==status::closed);
    CHECK(reentrant.state()==m::transfer_receiver::phase::aborted);
    m::decision move(identity);
    CHECK(move.commit(identity)==status::pending);CHECK(move.frozen(identity,1)==status::pending);
    CHECK(move.prepared(identity)==status::pending);CHECK(move.frozen(identity,0)==status::ok);
    CHECK(move.prepared(identity)==status::ok);CHECK(move.commit(identity)==status::ok);
    CHECK(move.abort(identity)==status::conflict);CHECK(move.activated(identity)==status::pending);
    CHECK(move.fenced(different)==status::conflict);CHECK(move.fenced(identity)==status::ok);
    CHECK(move.activated(identity)==status::ok && move.ready() && move.cleanup_pending());
    CHECK(move.retired(identity)==status::ok && !move.cleanup_pending());
    m::decision cancelled(identity);CHECK(cancelled.abort(identity)==status::ok);
    CHECK(cancelled.source_restored(identity)==status::pending);
    CHECK(cancelled.target_aborted(identity)==status::ok);CHECK(cancelled.source_restored(identity)==status::ok);
    CHECK(cancelled.aborted());CHECK(cancelled.commit(identity)==status::conflict);
    CHECK(cancelled.frozen(identity,0)==status::closed);
    std::cout<<"Snapshot framing, receiver retention and monotonic migration decision checks passed\n";
}
