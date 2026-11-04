#pragma once
#include <functional>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
namespace dynabridge { namespace probe_native {
struct counter {
    int n = 7;
    int value() const { return n; }
    int add(int v) { return n += v; }
};
}}
namespace probe_native = dynabridge::probe_native;
#define DYNABRIDGE_IMPORT_DEF "tests/probes/import.def"
#define DYNABRIDGE_EXPORT_DEF "tests/probes/export.def"
#include <dynabridge/bridge.h>
#undef DYNABRIDGE_IMPORT_DEF
#undef DYNABRIDGE_EXPORT_DEF
#include <dynabridge/backends/rpc.h>

namespace probe {
// P0 only: no transport, no grant protocol. Object keys remain in an object
// channel distinct from scalar conversion; native pointers never leave Host.
struct key { unsigned incarnation; unsigned id; };
struct backend : dynabridge::backend_base<backend> {
    using dynamic_value_t = dynabridge::rpc::value;
    template<class T> using converter = dynabridge::rpc_backend::converter<T>;
    struct entry { std::type_index type; void* proxy; };
    struct context {
        using backend_t = backend;
        std::unordered_map<unsigned, entry> registry;
        unsigned incarnation = 42;
        unsigned lower_count = 0, bind_count = 0;
        std::function<dynamic_value_t(key, const std::vector<dynamic_value_t>&)> call;
    };
    template<class Receiver, class Direction = typename dynabridge::bridge_direction<Receiver>::type>
    class object_t : public dynabridge::object_base_selector<object_t<Receiver, Direction>, backend, Receiver, Direction>::type {
        key key_;
    public:
        explicit object_t(key k) noexcept : key_(k) {}
        key get() const noexcept { return key_; }
        Receiver* native(context& c) const {
            auto it = c.registry.find(key_.id);
            if (key_.incarnation != c.incarnation || it == c.registry.end() || it->second.type != std::type_index(typeid(Receiver)))
                DYNABRIDGE_RAISE(std::runtime_error("wrong object identity/type"));
            return static_cast<Receiver*>(it->second.proxy);
        }
    };
    template<class Class, class Receiver>
    static dynamic_value_t to_dynamic_object_impl(context& c, const object_t<Receiver, dynabridge::import_t>& o) {
        ++c.lower_count;
        return dynamic_value_t::string(std::to_string(o.get().incarnation) + ":" + std::to_string(o.get().id));
    }
    static key decode(const dynamic_value_t& v) {
        if (v.kind() != dynabridge::rpc::value_kind::string) DYNABRIDGE_RAISE(std::runtime_error("not an object reference"));
        const auto& s=v.copy_string();
        auto colon=s.find(':');
        if (colon==std::string::npos) DYNABRIDGE_RAISE(std::runtime_error("bad object reference"));
        return {static_cast<unsigned>(std::stoul(s.substr(0,colon))), static_cast<unsigned>(std::stoul(s.substr(colon+1)))};
    }
    template<class Class>
    static object_t<Class, dynabridge::export_t> bind_export_object_impl(context& c, dynamic_value_t v) {
        auto k=decode(v);
        ++c.bind_count;
        object_t<Class, dynabridge::export_t> o(k);
        (void)o.native(c); // Checked binding before the generated export binder.
        return o;
    }
    template<class R, class Receiver, class... Args>
    static R invoke_impl(context& c, dynamic_value_t v, Args... args) {
        return dynabridge::from_cast<R>(c, c.call(decode(v), {args...}));
    }
};
using delegate = decltype(dynabridge::bind_receiver<dynabridge::counter>(std::declval<backend::context&>(), key{42, 1}));
// Only read dispatch may mutate this private delegate's runtime bookkeeping.
// Writes are exposed solely on a non-const facade; no const_cast.
class facade {
    mutable delegate delegate_;
public:
    facade(backend::context& ctx, key k) : delegate_(dynabridge::bind_receiver<dynabridge::counter>(ctx, k)) {}
    int value() const { return delegate_.value(); }
    int add(int n) { return delegate_.add(n); }
};
}
