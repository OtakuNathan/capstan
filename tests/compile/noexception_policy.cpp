#if defined(PROBE_MIGRATION)
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
