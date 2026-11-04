#include "tests/probes/bridge.hpp"
#include <lease.hpp>
int main() { probe::backend::context c; probe::facade f(c,{42,1}); auto ro=lease::access::make_ro(f); ro->add(1); }
