#include "tests/probes/bridge.hpp"
#include <lease.hpp>
#include "tests/check.hpp"
int main() {
    probe::backend::context ctx;
    ctx.call = [](probe::key, const std::vector<dynabridge::rpc::value>& args) { CHECK(args.empty()); return dynabridge::rpc::value::signed_integer(23); };
    probe::facade object(ctx, {42, 1});
    auto rw = lease::access::make_rw<lease::access::shared>(object);
    auto ro = rw.borrow_ro();
    auto copy = ro;
    CHECK(ro->value() == 23 && copy->value() == 23);
    CHECK(ctx.lower_count == 2);
}
