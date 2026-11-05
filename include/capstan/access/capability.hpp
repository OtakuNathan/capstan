#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/bridge/dataset.hpp>
#include <capstan/async/io.hpp>
#include <lease.hpp>
#include <limits>
#include <algorithm>
#include <mutex>
namespace capstan {
class transfer_plan;
class cut_plan;
class capability_busy : public std::logic_error { public: capability_busy():std::logic_error("capability has active borrow/write or is closed"){} };
namespace access_detail {
struct backing {
    dataset_backend::context context;
    dataset_facade facade;
    std::atomic<bool> closed{false};
    explicit backing(grant g):facade(context,g) {}
    id request_id() {
        static std::atomic<id> next_request{1};
        auto n=next_request.load();
        for(;;) {
            if(n==std::numeric_limits<id>::max()) CAPSTAN_RAISE(std::overflow_error("request id exhausted"));
            if(next_request.compare_exchange_weak(n,n+1)) return n;
        }
    }
};
using read_lease=lease::access::shared_access<dataset_facade,lease::access::shared>;
using write_lease=lease::access::exclusive_access<dataset_facade,lease::access::shared>;
struct read_frame {
    std::shared_ptr<backing> storage;
    read_lease access;
    read_frame(std::shared_ptr<backing> b,read_lease r):storage(std::move(b)),access(std::move(r)) {}
};
struct exclusive_state {
    const grant identity;
    std::shared_ptr<backing> storage;
    std::unique_ptr<write_lease> write;
    std::mutex mutex;
    unsigned readers=0;
    bool closed=false;
    explicit exclusive_state(grant g):identity(g),storage(std::make_shared<backing>(g)),write(new write_lease(lease::access::make_rw<lease::access::shared>(storage->facade))) {}
};
struct registry_entry {grant identity; std::weak_ptr<exclusive_state> state; bool sealed=false;};
struct exclusive_registry {std::mutex mutex; std::vector<registry_entry> entries;};
inline exclusive_registry& registry() {static exclusive_registry value;return value;}
// Sealed entries are bounded tombstones: destroying all wrappers does not
// make a submitted source grant materializable again. No silent eviction.
inline bool seal(const std::shared_ptr<exclusive_state>& owner) noexcept {
    auto& local=registry();
    std::lock_guard<std::mutex> registry_lock(local.mutex);
    std::lock_guard<std::mutex> state_lock(owner->mutex);
    if(owner->closed) return false;
    for(auto& entry:local.entries) if(entry.identity==owner->identity) {
        entry.sealed=true;owner->closed=true;return true;
    }
    return false;
}
// Raw grants are materialized only once as a live exclusive session in this
// process. Weak entries include operation-held states after wrapper destruction.
// The registry is local topology bookkeeping: no acquire/release I/O occurs.
inline std::shared_ptr<exclusive_state> materialize_exclusive(grant g,status* error=nullptr) {
    if(error) *error=status::ok;
    if(g.mode!=access_mode::exclusive) {
        if(error) {*error=status::invalid;return {};}
        CAPSTAN_RAISE(std::invalid_argument("exclusive capability needs exclusive grant"));
    }
    auto& local=registry();
    std::lock_guard<std::mutex> lock(local.mutex);
    auto& entries=local.entries;
    entries.erase(std::remove_if(entries.begin(),entries.end(),[](const registry_entry& e){return !e.sealed && e.state.expired();}),entries.end());
    for(const auto& entry:entries) if(entry.identity==g) {
        if(error) {*error=status::duplicate;return {};}
        CAPSTAN_RAISE(std::logic_error("exclusive grant already materialized locally"));
    }
    if(entries.size()>=4096 || std::count_if(entries.begin(),entries.end(),[](const registry_entry& e){return !e.state.expired();})>=64) {
        if(error) {*error=status::exhausted;return {};}
        CAPSTAN_RAISE(std::length_error("local exclusive session bound reached"));
    }
    auto state=std::make_shared<exclusive_state>(g);
    entries.push_back({g,state,false});
    return state;
}
struct read_guard {
    std::shared_ptr<exclusive_state> owner;
    std::unique_ptr<read_lease> access;
    explicit read_guard(std::shared_ptr<exclusive_state> s):owner(std::move(s)) {
        std::lock_guard<std::mutex> lock(owner->mutex);
        if(owner->closed || !owner->write) return;
        access.reset(new read_lease(owner->write->borrow_ro()));
        ++owner->readers;
    }
    ~read_guard() {
        if(!access) return;
        std::lock_guard<std::mutex> lock(owner->mutex);
        access.reset();
        --owner->readers;
    }
};
struct write_guard {
    std::shared_ptr<exclusive_state> owner;
    std::unique_ptr<write_lease> access;
    explicit write_guard(std::shared_ptr<exclusive_state> s):owner(std::move(s)) {
        std::lock_guard<std::mutex> lock(owner->mutex);
        if(owner->closed || !owner->write || owner->readers) return;
        access=std::move(owner->write);
    }
    ~write_guard() {
        if(!access) return;
        std::lock_guard<std::mutex> lock(owner->mutex);
        owner->write=std::move(access);
    }
};
}
template<class R> struct invocation {
    typed_call<R> call;
    std::shared_ptr<void> authority;
    id request_id;
    invocation(typed_call<R> c,std::shared_ptr<void> a,id r):call(std::move(c)),authority(std::move(a)),request_id(r) {}
    invocation(const invocation&)=delete;
    invocation& operator=(const invocation&)=delete;
    invocation(invocation&&) noexcept=default;
};
class read_capability {
    std::shared_ptr<access_detail::backing> storage_;
    access_detail::read_lease access_;
public:
    explicit read_capability(grant g):storage_(std::make_shared<access_detail::backing>(g)),access_(lease::access::make_ro<lease::access::shared>(storage_->facade)) {
        if(g.mode!=access_mode::shared) CAPSTAN_RAISE(std::invalid_argument("read capability needs shared grant"));
    }
    read_capability(const read_capability&)=default;
    read_capability(read_capability&&) noexcept=default;
    dynabridge::optional<invocation<std::uint64_t>> try_version() const {
        if(!storage_ || storage_->closed.load()) return {};
        auto frame=std::make_shared<access_detail::read_frame>(storage_,access_);
        return dynabridge::optional<invocation<std::uint64_t>>(invocation<std::uint64_t>{frame->access->version(),frame,storage_->request_id()});
    }
    dynabridge::optional<invocation<summary_t>> try_summary() const {
        if(!storage_ || storage_->closed.load()) return {};
        auto frame=std::make_shared<access_detail::read_frame>(storage_,access_);
        return dynabridge::optional<invocation<summary_t>>(invocation<summary_t>{frame->access->summary(),frame,storage_->request_id()});
    }
    invocation<std::uint64_t> version() const {
        auto call=try_version();if(!call) CAPSTAN_RAISE(capability_busy());return std::move(*call);
    }
    invocation<summary_t> summary() const {
        auto call=try_summary();if(!call) CAPSTAN_RAISE(capability_busy());return std::move(*call);
    }
    bool try_close(io_endpoint& control,bytes request,io_completion done) {
        if(!storage_ || storage_->closed.exchange(true)) return false;
        start_io(control,std::move(request),std::move(done),storage_);return true;
    }
    void close(io_endpoint& control,bytes request,io_completion done) {
        if(!try_close(control,std::move(request),std::move(done))) CAPSTAN_RAISE(capability_busy());
    }
};
class exclusive_capability {
    friend class transfer_plan;
    friend class cut_plan;
    std::shared_ptr<access_detail::exclusive_state> owner_;
    explicit exclusive_capability(std::shared_ptr<access_detail::exclusive_state> owner):owner_(std::move(owner)) {}
public:
    explicit exclusive_capability(grant g):owner_(access_detail::materialize_exclusive(g)) {}
    static std::unique_ptr<exclusive_capability> try_create(grant g,status& code) {
        auto owner=access_detail::materialize_exclusive(g,&code);
        if(!owner) return {};
        return std::unique_ptr<exclusive_capability>(new exclusive_capability(std::move(owner)));
    }
    exclusive_capability(const exclusive_capability&)=delete;
    exclusive_capability& operator=(const exclusive_capability&)=delete;
    exclusive_capability(exclusive_capability&&) noexcept=default;
    dynabridge::optional<invocation<std::uint64_t>> try_version() const {
        if(!owner_) return {};
        auto frame=std::make_shared<access_detail::read_guard>(owner_);
        if(!frame->access) return {};
        return dynabridge::optional<invocation<std::uint64_t>>(invocation<std::uint64_t>{(*frame->access)->version(),frame,owner_->storage->request_id()});
    }
    dynabridge::optional<invocation<summary_t>> try_summary() const {
        if(!owner_) return {};
        auto frame=std::make_shared<access_detail::read_guard>(owner_);
        if(!frame->access) return {};
        return dynabridge::optional<invocation<summary_t>>(invocation<summary_t>{(*frame->access)->summary(),frame,owner_->storage->request_id()});
    }
    dynabridge::optional<invocation<std::uint64_t>> try_add_range(std::uint64_t b,std::uint64_t e,std::uint64_t d) {
        if(!owner_) return {};
        auto frame=std::make_shared<access_detail::write_guard>(owner_);
        if(!frame->access) return {};
        return dynabridge::optional<invocation<std::uint64_t>>(invocation<std::uint64_t>{(*frame->access)->add_range(b,e,d),frame,owner_->storage->request_id()});
    }
    invocation<std::uint64_t> version() const {
        auto call=try_version();if(!call) CAPSTAN_RAISE(capability_busy());return std::move(*call);
    }
    invocation<summary_t> summary() const {
        auto call=try_summary();if(!call) CAPSTAN_RAISE(capability_busy());return std::move(*call);
    }
    bool try_close(io_endpoint& control,bytes request,io_completion done) {
        if(!owner_) return {};
        {
            std::lock_guard<std::mutex> lock(owner_->mutex);
            if(owner_->closed || !owner_->write || owner_->readers) return false;
            owner_->closed=true;
        }
        start_io(control,std::move(request),std::move(done),owner_);return true;
    }
    invocation<std::uint64_t> add_range(std::uint64_t begin,std::uint64_t end,std::uint64_t delta) {
        auto call=try_add_range(begin,end,delta);if(!call) CAPSTAN_RAISE(capability_busy());return std::move(*call);
    }
    void close(io_endpoint& control,bytes request,io_completion done) {
        if(!try_close(control,std::move(request),std::move(done))) CAPSTAN_RAISE(capability_busy());
    }
};
// Result categories remain explicit. Decode typed values only on Completed/ok.
template<class R> void start_call(io_endpoint& endpoint,invocation<R>&& call,std::function<void(wire::reply)> done) {
    auto frame=call.call.encode(call.request_id);
    start_io(endpoint,std::move(frame),[done=std::move(done)](io_result io) mutable {
        wire::reply reply{io.state,status::invalid,{}};
        if(io.state==outcome::completed) {
            if(!wire::try_decode_reply(io.payload,reply)) reply={outcome::unknown,status::invalid,{}};
        }
        done(std::move(reply));
    },std::move(call.authority));
}
// Access is acquired by the existing capability facade; generated calls do not infer effects.
template<class R> auto bind_async_operation(io_endpoint& endpoint,invocation<R>&& pending) {
    auto context=std::make_shared<dataset_backend::context>();
    context->async_endpoint=&endpoint;context->async_request_id=pending.request_id;
    context->async_authority=std::move(pending.authority);
#ifdef CAPSTAN_ERASED_INVOCATIONS
    return dynabridge::bind_async<dynabridge::dataset>(context,
        dataset_delegate::object_t(pending.call.header.capability));
#else
    return dynabridge::bind_static_async<dynabridge::dataset>(context,
        dataset_delegate::object_t(pending.call.header.capability));
#endif
}
class async_read_capability {
    std::shared_ptr<read_capability> access_;
    io_endpoint* endpoint_;
public:
    async_read_capability(std::shared_ptr<read_capability> access,io_endpoint& endpoint)
        :access_(std::move(access)),endpoint_(&endpoint) {dynabridge::async_detail::require(bool(access_));}
    auto version() const {return bind_async_operation(*endpoint_,access_->version()).version();}
    auto summary() const {return bind_async_operation(*endpoint_,access_->summary()).summary();}
};
class async_exclusive_capability {
    std::shared_ptr<exclusive_capability> access_;
    io_endpoint* endpoint_;
public:
    async_exclusive_capability(std::shared_ptr<exclusive_capability> access,io_endpoint& endpoint)
        :access_(std::move(access)),endpoint_(&endpoint) {dynabridge::async_detail::require(bool(access_));}
    auto version() const {return bind_async_operation(*endpoint_,access_->version()).version();}
    auto summary() const {return bind_async_operation(*endpoint_,access_->summary()).summary();}
    auto add_range(std::uint64_t b,std::uint64_t e,std::uint64_t d) {
        return bind_async_operation(*endpoint_,access_->add_range(b,e,d)).add_range(b,e,d);
    }
};

}
