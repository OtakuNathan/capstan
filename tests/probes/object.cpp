#include "tests/probes/bridge.hpp"
#include "tests/check.hpp"
int main() {
    probe::backend::context ctx;
    probe_native::counter native;
    portal::exports::counter proxy(portal::borrow(native));
    ctx.registry.emplace(1, probe::backend::entry{typeid(decltype(proxy)), &proxy});
    auto read = portal::create_export_member_callable_binder<portal::exports::counter, int()>(ctx, &probe_native::counter::value);
    auto write = portal::create_export_member_callable_binder<portal::exports::counter, int(int)>(ctx, &probe_native::counter::add);
    ctx.call = [&](probe::key k, const std::vector<portal::rpc::value>& a) {
        auto v = portal::rpc::value::string(std::to_string(k.incarnation) + ":" + std::to_string(k.id));
        return a.empty() ? read(v) : write(v, a.at(0));
    };
    auto object = portal::bind_receiver<portal::counter>(ctx, probe::key{42, 1});
    CHECK(object.value() == 7);
    CHECK(object.add(5) == 12);
    CHECK(ctx.lower_count == 2 && ctx.bind_count == 2);
    bool rejected = false;
    rejected=rejects<std::runtime_error>([&]{ (void)read(portal::rpc::value::string("43:1")); });
    CHECK(rejected);
    rejected = false;
    rejected=rejects<portal::bad_conversion>([&]{ (void)write(portal::rpc::value::string("42:1"), portal::rpc::value::boolean(true)); });
    CHECK(rejected && native.n == 12);
    std::cout << "typed receiver lowered, checked export bind, native member and strict scalar conversion passed\n";
}
