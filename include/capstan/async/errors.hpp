#pragma once
#include <base/traits.h>
#include <flow/flow_def.h>
#include <system_error>
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
namespace flux_foundry {
template<> struct cancel_error<std::error_code> {
    static std::error_code make(cancel_kind) noexcept {return std::make_error_code(std::errc::operation_canceled);}
};
template<> struct awaitable_creating_error<std::error_code> {
    static std::error_code make() noexcept {return std::make_error_code(std::errc::not_enough_memory);}
};
template<> struct async_submission_failed_error<std::error_code> {
    static std::error_code make() noexcept {return std::make_error_code(std::errc::resource_unavailable_try_again);}
};
}
#endif
