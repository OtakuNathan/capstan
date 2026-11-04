#include <capstan/bridge/dataset.hpp>
void bad(capstan::dataset_backend::object_t<dynabridge::other_resource<capstan::dataset_backend::context>,dynabridge::import_t> other) {
    capstan::dataset_delegate::object_t dataset=other;
}
