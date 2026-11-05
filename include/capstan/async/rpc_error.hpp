#pragma once
#include <capstan/protocol/types.hpp>
#include <dynabridge/async.h>
namespace capstan {
class rpc_error_category final : public std::error_category {
public:
    const char* name() const noexcept override {return "capstan.rpc";}
    std::string message(int code) const override {return "Capstan outcome/status " + std::to_string(code);}
};
inline const std::error_category& rpc_category() noexcept {static rpc_error_category category;return category;}
inline dynabridge::async_error make_rpc_error(outcome state,status code) noexcept {
    return std::error_code(1+static_cast<int>(state)*256+static_cast<int>(code),rpc_category());
}
inline bool decode_rpc_error(const dynabridge::async_error& error,outcome& state,status& code) noexcept {
    if(&error.code.category()!=&rpc_category() || error.code.value()<1) return false;
    const auto raw=error.code.value()-1;
    if(raw/256>static_cast<int>(outcome::unknown) || raw%256>static_cast<int>(status::superseded)) return false;
    state=static_cast<outcome>(raw/256);code=static_cast<status>(raw%256);return true;
}
}
