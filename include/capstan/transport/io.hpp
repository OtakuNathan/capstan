#pragma once
#include <capstan/protocol/types.hpp>
#include <dynabridge/backends/rpc.h>
#include <functional>
#include <memory>
#include <utility>
namespace capstan {
using bytes = dynabridge::rpc::bytes;
struct io_result { outcome state=outcome::unknown; bytes payload; };
using io_completion = std::function<void(io_result)>;
// Submission is local/nonblocking. true -> exactly one completion (may inline).
// false -> no completion ever. Disconnect after submit -> unknown, not rejected.
// Callers keep endpoint alive until all submissions finish; v0 has no hard cancel.
class io_endpoint {
public:
    virtual ~io_endpoint() = default;
    virtual bool submit(bytes request, io_completion completion) noexcept = 0;
    // Optional ownership handoff for adapters which detach the local waiter.
    // Ordinary endpoints need no extra owner: the FF operation already retains
    // it. Drop this temporary copy before submit so inline completion can return
    // an exclusive token before a reentrant call from the user's callback.
    virtual bool submit_owned(bytes request, io_completion completion, std::shared_ptr<void> lifetime) noexcept {
        lifetime.reset();
        return submit(std::move(request), std::move(completion));
    }
};
}
