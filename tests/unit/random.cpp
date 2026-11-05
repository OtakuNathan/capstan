#include <capstan/protocol/random.hpp>
#include "tests/check.hpp"
#include <algorithm>
#include <array>
#include <utility>

int main() {
    CHECK(capstan::try_os_random(nullptr,0));
    std::array<unsigned char,602> buffer;
    buffer.fill(0xa5);
    CHECK(capstan::try_os_random(buffer.data()+1,600));
    CHECK(buffer.front()==0xa5 && buffer.back()==0xa5);
    // Exercise Darwin's 256-byte boundary and the final partial chunk.
    for(auto range:{std::make_pair(1,257),std::make_pair(257,513),std::make_pair(513,601)})
        CHECK(!std::all_of(buffer.begin()+range.first,buffer.begin()+range.second,
                          [](unsigned char value){return value==0xa5;}));
    capstan::token token{};capstan::id id=0;
    CHECK(capstan::try_random_token(token) && token!=capstan::token{});
    CHECK(capstan::try_random_id(id) && id!=0);
}
