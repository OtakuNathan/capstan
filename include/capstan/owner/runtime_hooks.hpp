#pragma once
#include <capstan/owner/backend.hpp>
#include <capstan/authority/host.hpp>
namespace capstan {
// Optional persistence policy. Memory backends pay no storage barrier and keep
// their existing dispatch contract. Durable specializations receive request IDs.
template<class Backend> struct owner_runtime_hooks {
    static constexpr bool durable=false;
    static bool initialize(Backend&,host_authority&,id,std::size_t) PORTAL_NOEXCEPT_IF_DISABLED {return true;}
    static bool checkpoint(Backend&,host_authority&) PORTAL_NOEXCEPT_IF_DISABLED {return true;}
    static bool healthy(const Backend&) noexcept {return true;}
    static bool allows(id) noexcept {return true;}
    static bool replay(Backend&,const wire::call&,id,bytes&) PORTAL_NOEXCEPT_IF_DISABLED {return false;}
    static bytes execute(Backend&,typename Backend::object_type& owner,const wire::call& call) PORTAL_NOEXCEPT_IF_DISABLED {
        wire::value result;
        bool ok=owner.try_dispatch(call.header.operation,call.header.capability,call.args,result);
        return wire::encode_reply(outcome::completed,ok ? status::ok : status::invalid,ok ? std::vector<wire::value>{result} : std::vector<wire::value>{});
    }
};
template<class B> struct owner_runtime_contract : owner_runtime_hooks<B> {
    using H=owner_runtime_hooks<B>;using O=typename B::object_type;
    static_assert(std::is_same<decltype(H::initialize(std::declval<B&>(),std::declval<host_authority&>(),id{},std::size_t{})),bool>::value &&
                  std::is_same<decltype(H::checkpoint(std::declval<B&>(),std::declval<host_authority&>())),bool>::value &&
                  std::is_same<decltype(H::replay(std::declval<B&>(),std::declval<const wire::call&>(),id{},std::declval<bytes&>())),bool>::value &&
                  std::is_same<decltype(H::execute(std::declval<B&>(),std::declval<O&>(),std::declval<const wire::call&>())),bytes>::value &&
                  std::is_same<decltype(H::healthy(std::declval<const B&>())),bool>::value && std::is_same<decltype(H::allows(id{})),bool>::value,
                  "owner runtime hooks must return bool or encoded bytes as specified");
    static_assert(noexcept(H::healthy(std::declval<const B&>())) && noexcept(H::allows(id{})),"runtime health/allowlist hooks must be noexcept");
#if !CAPSTAN_HAS_EXCEPTIONS && (!defined(CAPSTAN_NO_EXCEPTION_STRICT) || CAPSTAN_NO_EXCEPTION_STRICT)
    static_assert(noexcept(H::initialize(std::declval<B&>(),std::declval<host_authority&>(),id{},std::size_t{})) &&
                  noexcept(H::checkpoint(std::declval<B&>(),std::declval<host_authority&>())) &&
                  noexcept(H::replay(std::declval<B&>(),std::declval<const wire::call&>(),id{},std::declval<bytes&>())) &&
                  noexcept(H::execute(std::declval<B&>(),std::declval<O&>(),std::declval<const wire::call&>())),
                  "strict owner runtime hooks must be noexcept");
#endif
};
}
