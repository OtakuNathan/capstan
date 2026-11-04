#include "tests/probes/bridge.hpp"
int main() { probe::backend::context c; const auto o=dynabridge::bind_receiver<dynabridge::counter>(c,probe::key{42,1}); o.value(); }
