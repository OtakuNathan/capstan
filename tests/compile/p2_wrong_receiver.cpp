#include <capstan/bridge/dataset.hpp>
void bad(capstan::dataset_backend::object_t<portal::other_resource<capstan::dataset_backend::context>,portal::import_t> other) {
    capstan::dataset_delegate::object_t dataset=other;
}
