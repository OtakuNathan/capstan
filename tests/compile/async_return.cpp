#include "tests/probes/bridge.hpp"
#include <future>
// The generated contract's int return cannot become a future via invoke_impl.
std::future<int> call(probe::delegate& object) { return object.value(); }
