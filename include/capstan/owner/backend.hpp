#pragma once
#include <capstan/protocol/wire.hpp>
#include <capstan/migration/snapshot.hpp>
#include <memory>
#include <type_traits>
#include <utility>

namespace capstan {
// Borrowed input to synchronous restore. The restored object must own/retain
// everything it needs before restore returns; no transport buffer is retained.
struct owner_snapshot_view {const std::uint8_t* data;std::size_t size;};

// A structural contract: no inheritance, vtable or callable erasure required.
// Backend owns creation/restoration policy; object_type owns one logical object.
// See docs/owner-backends.md for completion, pin and publication obligations.
template<class Backend> struct owner_backend_contract {
    using object_type=typename Backend::object_type;
    using pointer=std::unique_ptr<object_type>;
    using B=Backend;using O=object_type;
    static_assert(std::is_same<decltype(std::declval<B&>().create(object_key{},std::size_t{},id{})),pointer>::value,
                  "owner backend create must return unique_ptr<object_type>");
    static_assert(std::is_same<decltype(std::declval<B&>().restore(object_key{},owner_snapshot_view{},std::declval<pointer&>())),status>::value,
                  "owner backend restore must return status");
    static_assert(std::is_same<decltype(std::declval<O&>().try_dispatch(method{},std::declval<const grant&>(),std::declval<const std::vector<wire::value>&>(),std::declval<wire::value&>())),bool>::value,
                  "owner object try_dispatch must return bool");
    static_assert(std::is_same<decltype(std::declval<const O&>().capture_snapshot(std::declval<migration::buffer&>())),status>::value &&
                  std::is_same<decltype(std::declval<const O&>().append_snapshot(std::declval<migration::buffer&>())),status>::value,
                  "owner object capture/append must return status");
    static_assert(std::is_same<decltype(std::declval<const O&>().resource_bytes()),std::size_t>::value &&
                  std::is_same<decltype(std::declval<const O&>().snapshot_bytes()),std::size_t>::value &&
                  std::is_same<decltype(std::declval<const B&>().restore_reservation(std::size_t{})),std::size_t>::value,
                  "owner size/reservation methods must return size_t");
    static_assert(std::is_same<decltype(std::declval<const B&>().can_inline(method{})),bool>::value,
                  "owner can_inline must return bool");
    static_assert(noexcept(std::declval<const O&>().resource_bytes()) && noexcept(std::declval<const O&>().snapshot_bytes()) &&
                  noexcept(std::declval<const B&>().restore_reservation(std::size_t{})) &&
                  noexcept(std::declval<const B&>().can_inline(method{})),
                  "owner sizing and dispatch hints must be noexcept");
    static_assert(std::is_convertible<decltype(B::concurrent_read_capture),bool>::value,
                  "owner backend must explicitly declare concurrent_read_capture");
    static_assert(std::is_nothrow_destructible<O>::value && std::is_nothrow_destructible<B>::value,
                  "owner backend and object destruction must not throw");
#if !CAPSTAN_HAS_EXCEPTIONS && (!defined(CAPSTAN_NO_EXCEPTION_STRICT) || CAPSTAN_NO_EXCEPTION_STRICT)
    static_assert(noexcept(std::declval<B&>().create(object_key{},std::size_t{},id{})) &&
                  noexcept(std::declval<B&>().restore(object_key{},owner_snapshot_view{},std::declval<pointer&>())) &&
                  noexcept(std::declval<const O&>().capture_snapshot(std::declval<migration::buffer&>())) &&
                  noexcept(std::declval<const O&>().append_snapshot(std::declval<migration::buffer&>())) &&
                  noexcept(std::declval<O&>().try_dispatch(method{},std::declval<const grant&>(),std::declval<const std::vector<wire::value>&>(),std::declval<wire::value&>())),
                  "strict owner backend create/restore/capture/dispatch must be noexcept");
#endif
};
template<class Backend> std::shared_ptr<Backend> require_owner_backend(std::shared_ptr<Backend> backend) {
    if(!backend) CAPSTAN_RAISE(std::invalid_argument("owner backend is null"));
    return backend;
}
}
