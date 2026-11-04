#include <capstan/access/capability.hpp>
void bad(capstan::read_capability& a) { auto call=a.add_range(0,1,1); }
