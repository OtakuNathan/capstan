#pragma once
#include <cstdint>
namespace capstan {
struct summary_t {
    std::uint64_t count=0, sum=0, checksum=0, version=0;
    bool operator==(const summary_t& b) const { return count==b.count && sum==b.sum && checksum==b.checksum && version==b.version; }
};
}
