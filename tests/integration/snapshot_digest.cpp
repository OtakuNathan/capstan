#include <capstan/migration/gio_digest.hpp>
#include "tests/check.hpp"
int main() {
    using namespace capstan;using namespace capstan::migration;
    buffer abc{'a','b','c'};
    const std::array<std::uint8_t,32> expected{{0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad}};
    CHECK(sha256(abc)==expected);
    manifest m;m.transaction={1,1};m.object={1,1,1};m.source_host=1;m.target_host=2;
    m.source_instance=1;m.target_instance=2;m.generation=1;m.size=3;m.digest=expected;
    transfer_receiver bad;CHECK(bad.begin(m)==status::ok);
    abc[0]='x';CHECK(bad.append(m,0,abc)==status::ok);
    auto validate=[](const buffer& data,const std::array<std::uint8_t,32>& digest) noexcept {return sha256(data)==digest ? status::ok : status::invalid;};
    CHECK(bad.seal(m,validate)==status::invalid && !bad.prepared_data());
    transfer_receiver good;CHECK(good.begin(m)==status::ok);
    abc[0]='a';CHECK(good.append(m,0,abc)==status::ok);CHECK(good.seal(m,validate)==status::ok);
    // Reverse-order chunks, holes, identical/conflicting duplicates, retry and
    // full digest compatibility with the original SHA-256 package commitment.
    buffer payload(transfer_receiver::chunk_bytes*2+17);
    for(std::size_t i=0;i<payload.size();++i) payload[i]=static_cast<std::uint8_t>(i*31);
    m.size=payload.size();m.digest=sha256(payload);
    std::array<std::uint8_t,32> reference{};gsize reference_size=reference.size();
    auto* independent=g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(independent,payload.data(),static_cast<gssize>(payload.size()));
    g_checksum_get_digest(independent,reference.data(),&reference_size);g_checksum_free(independent);
    CHECK(m.digest==reference); // Cross-provider commitment, not only a round trip.
    transfer_receiver reordered(payload.size(),make_sha256_stream());CHECK(reordered.begin(m)==status::ok);
    const auto chunk=transfer_receiver::chunk_bytes;
    CHECK(reordered.append(m,chunk*2,payload.data()+chunk*2,17)==status::ok);
    CHECK(reordered.contiguous_bytes()==0 && !reordered.digest_valid());
    auto schema=[](const buffer&,const std::array<std::uint8_t,32>&) noexcept {return status::ok;};
    CHECK(reordered.seal(m,schema)==status::pending);
    CHECK(reordered.append(m,0,payload.data(),chunk)==status::ok);
    CHECK(reordered.contiguous_bytes()==chunk);
    CHECK(reordered.begin(m)==status::ok); // Retry preserves the digest prefix.
    CHECK(reordered.append(m,0,payload.data(),chunk)==status::ok);
    payload[0]^=1;CHECK(reordered.append(m,0,payload.data(),chunk)==status::conflict);payload[0]^=1;
    auto other=m;++other.generation;
    CHECK(reordered.append(other,chunk,payload.data()+chunk,chunk)==status::conflict);
    CHECK(reordered.append(m,chunk,payload.data()+chunk,chunk)==status::ok);
    CHECK(reordered.digest_valid() && reordered.contiguous_bytes()==payload.size());
    CHECK(reordered.seal(m,schema)==status::ok && *reordered.prepared_data()==payload);
    transfer_receiver corrupt(payload.size(),make_sha256_stream());CHECK(corrupt.begin(m)==status::ok);
    payload[0]^=1;
    for(std::size_t offset=0;offset<payload.size();offset+=chunk)
        CHECK(corrupt.append(m,offset,payload.data()+offset,std::min(chunk,payload.size()-offset))==status::ok);
    CHECK(corrupt.seal(m,schema)==status::invalid); // Cannot bypass digest with a permissive schema validator.
    CHECK(corrupt.abort(m)==status::ok);
    CHECK(corrupt.append(m,0,payload.data(),chunk)==status::closed);

}
