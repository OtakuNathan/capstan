#include "tests/probes/bridge.hpp"
int main() {
    probe::backend::object_t<portal::import_symbols::counter, portal::import_t> a(probe::key{42,1});
    probe::backend::object_t<portal::import_symbols::other, portal::import_t> b = a;
}
