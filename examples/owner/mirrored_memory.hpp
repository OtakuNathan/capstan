#pragma once
#include "examples/owner/single_process.hpp"
#include <atomic>
#include <limits>

namespace capstan { namespace examples {
// Demonstrates multiple private copies behind ONE logical owner. Both copies
// live in one process; this is not crash-tolerant database replication.
struct mirrored_owner_audit {
    std::atomic<std::size_t> created{0},restored{0},live{0},writes{0};
};
class mirrored_memory_object {
    object_key key_;
    std::unique_ptr<dynabridge::capstan_native::dataset> first_,second_;
    std::shared_ptr<mirrored_owner_audit> audit_;
public:
    mirrored_memory_object(object_key key,std::unique_ptr<dynabridge::capstan_native::dataset> first,
                           std::unique_ptr<dynabridge::capstan_native::dataset> second,std::shared_ptr<mirrored_owner_audit> audit) noexcept
        :key_(key),first_(std::move(first)),second_(std::move(second)),audit_(std::move(audit)) {++audit_->live;}
    ~mirrored_memory_object() noexcept {--audit_->live;}
    std::size_t resource_bytes() const noexcept {return 2*first_->resource_bytes();}
    std::size_t snapshot_bytes() const noexcept {return first_->resource_bytes()+64;}
    bool replicas_equal() const noexcept {return first_->summary()==second_->summary();}
    status append_snapshot(migration::buffer& out) const DYNABRIDGE_NOEXCEPT_IF_DISABLED {
        migration::put(out,0x314c5052u,4); // RPL1: opaque to Host, unlike CPSN.
        return first_->append_snapshot(out);
    }
    status capture_snapshot(migration::buffer& out) const DYNABRIDGE_NOEXCEPT_IF_DISABLED {
        migration::buffer staged;auto code=append_snapshot(staged);
        if(code==status::ok) out=std::move(staged);
        return code;
    }
    bool try_dispatch(method which,const grant& g,const std::vector<wire::value>& args,wire::value& out) DYNABRIDGE_NOEXCEPT_IF_DISABLED {
        if(g.key!=key_) return false;
        if(which==method::version && args.empty()) {out=wire::u(first_->version());return true;}
        if(which==method::summary && args.empty()) {
            dataset_backend::context conversion;
            out=dataset_backend::converter<summary_t>::to(conversion,first_->summary());return true;
        }
        if(which!=method::add_range || args.size()!=3) return false;
        for(const auto& arg:args) if(!wire::is_number(arg)) return false;
        const auto begin=wire::number(args[0]),end=wire::number(args[1]),delta=wire::number(args[2]);
        // Validate both copies before either mutation. After validation these
        // bounded in-memory updates do not allocate or fail in a live process.
        if(!first_->can_add_range(begin,end) || !second_->can_add_range(begin,end)) return false;
        const auto version=first_->add_range(begin,end,delta);
        second_->add_range(begin,end,delta);++audit_->writes;out=wire::u(version);return true;
    }
};
struct mirrored_memory_owner_backend {
    using object_type=mirrored_memory_object;
    std::shared_ptr<mirrored_owner_audit> audit;
    explicit mirrored_memory_owner_backend(std::shared_ptr<mirrored_owner_audit> state):audit(std::move(state)) {}
    // Deliberately conservative: even version() stays on the worker, and no
    // read/capture overlap is promised by this backend.
    static constexpr bool concurrent_read_capture=false;
    bool can_inline(method) const noexcept {return false;}
    std::size_t restore_reservation(std::size_t bytes) const noexcept {
        return bytes>std::numeric_limits<std::size_t>::max()/2 ? std::numeric_limits<std::size_t>::max() : bytes*2;
    }
    std::unique_ptr<object_type> create(object_key key,std::size_t bytes,id seed) DYNABRIDGE_NOEXCEPT_IF_DISABLED {
        using native=dynabridge::capstan_native::dataset;
        auto first=std::unique_ptr<native>(new native(bytes,seed)),second=std::unique_ptr<native>(new native(bytes,seed));
        auto object=std::unique_ptr<object_type>(new object_type(key,std::move(first),std::move(second),audit));
        ++audit->created;return object;
    }
    status restore(object_key key,owner_snapshot_view view,std::unique_ptr<object_type>& out) DYNABRIDGE_NOEXCEPT_IF_DISABLED {
        migration::reader input(view.data,view.size);std::uint64_t magic;
        if(!input.get(4,magic) || magic!=0x314c5052u) return status::invalid;
        migration::dataset_snapshot data;auto code=migration::decode(input.current(),input.remaining(),data);
        if(code!=status::ok) return code;
        auto copy=data;
        auto first=dynabridge::capstan_native::dataset::restore(std::move(data));
        auto second=dynabridge::capstan_native::dataset::restore(std::move(copy));
        if(!first || !second) return status::invalid;
        auto staged=std::unique_ptr<object_type>(new object_type(key,std::move(first),std::move(second),audit));
        out=std::move(staged);++audit->restored;return status::ok;
    }
};
}}
