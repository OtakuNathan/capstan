#include "tests/probes/bridge.hpp"
int main() { probe::backend::context c; const auto o=portal::bind_receiver<portal::counter>(c,probe::key{42,1}); o.value(); }
