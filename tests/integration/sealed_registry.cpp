#include <capstan/access/transfer.hpp>
#include "tests/check.hpp"
using namespace capstan;
struct refusing_endpoint:io_endpoint {bool submit(bytes,io_completion) noexcept override {return false;}};
int main() {
    refusing_endpoint endpoint;
    grant g{{44,55,66},1,1,1,access_mode::exclusive,{}};g.secret[0]=1;
    const auto first=g;unsigned completed=0;status code;
    for(unsigned n=0;n<4096;++n) {
        g.epoch=n+1;g.grant_id=n+1;
        auto capability=exclusive_capability::try_create(g,code);CHECK(capability);
        auto plan=transfer_plan::prepare(*capability,2,n+1,code);CHECK(plan);
        plan->start(endpoint,[&](io_result r){CHECK(r.state==outcome::not_submitted);++completed;});
        CHECK(plan->sealed() && !capability->try_version());
    }
    CHECK(completed==4096);
    CHECK(!exclusive_capability::try_create(first,code) && code==status::duplicate);
    ++g.epoch;++g.grant_id;
    CHECK(!exclusive_capability::try_create(g,code) && code==status::exhausted);
}
