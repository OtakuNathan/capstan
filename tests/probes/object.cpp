#include "tests/probes/bridge.hpp"
#include "tests/check.hpp"
int main() {
    probe::backend::context ctx;
    probe_native::counter native;
    dynabridge::exports::counter proxy(dynabridge::borrow(native));
    ctx.registry.emplace(1, probe::backend::entry{typeid(decltype(proxy)), &proxy});
    auto read = dynabridge::create_export_member_callable_binder<dynabridge::exports::counter, int()>(ctx, &probe_native::counter::value);
    auto write = dynabridge::create_export_member_callable_binder<dynabridge::exports::counter, int(int)>(ctx, &probe_native::counter::add);
    ctx.call = [&](probe::key k, const std::vector<dynabridge::rpc::value>& a) {
        auto v = dynabridge::rpc::value::string(std::to_string(k.incarnation) + ":" + std::to_string(k.id));
        return a.empty() ? read(v) : write(v, a.at(0));
    };
    auto object = dynabridge::bind_receiver<dynabridge::counter>(ctx, probe::key{42, 1});
    CHECK(object.value() == 7);
    CHECK(object.add(5) == 12);
    CHECK(ctx.lower_count == 2 && ctx.bind_count == 2);
    bool rejected = false;
    rejected=rejects<std::runtime_error>([&]{ (void)read(dynabridge::rpc::value::string("43:1")); });
    CHECK(rejected);
    rejected = false;
    rejected=rejects<dynabridge::bad_conversion>([&]{ (void)write(dynabridge::rpc::value::string("42:1"), dynabridge::rpc::value::boolean(true)); });
    CHECK(rejected && native.n == 12);
    std::cout << "typed receiver lowered, checked export bind, native member and strict scalar conversion passed\n";
}
