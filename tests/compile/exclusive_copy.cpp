#include <lease.hpp>
int main() { int x=0; auto rw=lease::access::make_rw(x); auto copy=rw; }
