#if defined(PROBE_OWNER)
#include <capstan/owner/backend.hpp>
#ifdef PROBE_NOEXCEPT
#define OWNER_EXTENSION_NOEXCEPT noexcept
#else
#define OWNER_EXTENSION_NOEXCEPT
#endif
struct owner_object {
    std::size_t resource_bytes() const noexcept;
    std::size_t snapshot_bytes() const noexcept;
    capstan::status capture_snapshot(capstan::migration::buffer&) const OWNER_EXTENSION_NOEXCEPT;
    capstan::status append_snapshot(capstan::migration::buffer&) const OWNER_EXTENSION_NOEXCEPT;
    bool try_dispatch(capstan::method,const capstan::grant&,const std::vector<capstan::wire::value>&,capstan::wire::value&) OWNER_EXTENSION_NOEXCEPT;
};
struct owner_backend {
    using object_type=owner_object;
    static constexpr bool concurrent_read_capture=false;
    bool can_inline(capstan::method) const noexcept;
    std::size_t restore_reservation(std::size_t) const noexcept;
    std::unique_ptr<object_type> create(capstan::object_key,std::size_t,capstan::id) OWNER_EXTENSION_NOEXCEPT;
    capstan::status restore(capstan::object_key,capstan::owner_snapshot_view,std::unique_ptr<object_type>&) OWNER_EXTENSION_NOEXCEPT;
};
int main() {return sizeof(capstan::owner_backend_contract<owner_backend>)==0;}
#elif defined(PROBE_MIGRATION)
#include <capstan/migration/transfer.hpp>
struct validator {
    capstan::status operator()(const capstan::migration::buffer&,const std::array<std::uint8_t,32>&)
#ifdef PROBE_NOEXCEPT
        noexcept
#endif
    {return capstan::status::ok;}
};
int main() {
    capstan::migration::transfer_receiver receiver;
    capstan::migration::manifest manifest;
    return static_cast<int>(receiver.seal(manifest,validator{}));
}
#elif defined(PROBE_BRIDGE)
#include <dynabridge/traits.h>
#include <dynabridge/optional.h>
struct backend {
    template<class T> struct converter {
        template<class C> static dynabridge::optional<T> from(C&,int value)
#ifdef PROBE_NOEXCEPT
            noexcept
#endif
        {return dynabridge::optional<T>(value);}
    };
};
// Preserve the backend context interface while checking the actual converter.
struct context { using backend_t=backend; };
int main() {context ctx;auto result=dynabridge::from_optional<int>(ctx,7);return result && *result==7 ? 0:1;}
#else
#include <base/traits.h>
#include <memory/inplace_t.h>
struct payload {
    explicit payload(int)
#ifdef PROBE_NOEXCEPT
        noexcept
#endif
    {}
};
int main() {
    alignas(payload) unsigned char storage[sizeof(payload)];
    auto* value=flux_foundry::raw_inplace_storage_operations<payload>::construct_at(storage,7);
    flux_foundry::raw_inplace_storage_operations<payload>::destroy_at(value);
}
#endif
