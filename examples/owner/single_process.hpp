#pragma once
#include <capstan/owner/backend.hpp>
#include <capstan/bridge/dataset.hpp>

namespace capstan { namespace examples {
// Default demo storage. This is one implementation of logical ownership, not
// the definition of an owner: another backend may wrap a database or cluster.
struct single_process_owner_backend {
    using object_type=dataset_binding;
    static constexpr bool concurrent_read_capture=true;
    bool can_inline(method operation) const noexcept {return operation==method::version;}
    std::size_t restore_reservation(std::size_t snapshot_size) const noexcept {return snapshot_size;}
    std::unique_ptr<object_type> create(object_key key,std::size_t bytes,id seed) DYNABRIDGE_NOEXCEPT_IF_DISABLED {
        return std::unique_ptr<object_type>(new object_type(key,bytes,seed));
    }
    status restore(object_key key,owner_snapshot_view bytes,std::unique_ptr<object_type>& out) DYNABRIDGE_NOEXCEPT_IF_DISABLED {
        migration::dataset_snapshot data;auto code=migration::decode(bytes.data,bytes.size,data);
        if(code!=status::ok) return code;
        auto native=dynabridge::capstan_native::dataset::restore(std::move(data));
        if(!native) return status::invalid;
        auto staged=std::unique_ptr<object_type>(new object_type(key,std::move(*native)));
        out=std::move(staged);return status::ok;
    }
};
}}
