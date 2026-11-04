#include <capstan/access/capability.hpp>
void bad(capstan::exclusive_capability& a) { auto copy=a; }
