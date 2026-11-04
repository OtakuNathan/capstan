#include "tests/probes/bridge.hpp"
int main() {
    probe::backend::object_t<dynabridge::import_symbols::counter, dynabridge::import_t> a(probe::key{42,1});
    probe::backend::object_t<dynabridge::import_symbols::other, dynabridge::import_t> b = a;
}
