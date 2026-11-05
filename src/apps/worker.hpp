#pragma once
#include <capstan/error_policy.hpp>
#include <executor/simple_executor.h>
#ifdef CAPSTAN_UV_WORKER
#include <executor/uv_executor.h>
#include <future>
#endif
#include <thread>
#include <memory>
#include <utility>
#include <type_traits>
#include <new>

namespace app {
// Ordinary business calls retain one serial execution lane per object. This
// adapter selects its FF worker; all network I/O stays on GIO. Migration may
// create a separate, explicitly pinned read-only snapshot lane below.
class business_worker {
#ifdef CAPSTAN_UV_WORKER
    uv_loop_t loop_{};
    std::unique_ptr<flux_foundry::uv_executor> executor_;
#else
    flux_foundry::simple_executor<64> executor_;
#endif
    std::thread thread_;
    int error_=0;
public:
    business_worker() {
#ifdef CAPSTAN_UV_WORKER
        std::promise<int> ready;
        auto started = ready.get_future();
        thread_ = std::thread([this, ready = std::move(ready)]() mutable {
            const int rc = uv_loop_init(&loop_);
            if (rc) {ready.set_value(rc);return;}
            CAPSTAN_TRY { executor_.reset(new flux_foundry::uv_executor(std::nothrow,&loop_,64,16)); }
            CAPSTAN_CATCH_ALL { uv_loop_close(&loop_);ready.set_value(UV_ENOMEM);return; }
            if(executor_->error()) {
                auto error=executor_->error();executor_.reset();uv_loop_close(&loop_);
                ready.set_value(error);return;
            }
            ready.set_value(0);
            uv_run(&loop_, UV_RUN_DEFAULT);
            if (uv_loop_close(&loop_)) std::terminate();
        });
        const int error=started.get();
        if(error) {thread_.join();error_=error;}
#else
        thread_ = std::thread([this] { executor_.run(); });
#endif
    }
    bool valid() const noexcept {return error_==0;}
    int error() const noexcept {return error_;}
    ~business_worker() { close(); }
    void close() noexcept {
        if (!thread_.joinable()) return;
#ifdef CAPSTAN_UV_WORKER
        executor_->close();
#else
        executor_.try_shutdown();
#endif
        thread_.join();
    }
    business_worker(const business_worker&) = delete;
    business_worker& operator=(const business_worker&) = delete;
    bool try_dispatch(flux_foundry::task_wrapper_sbo&& task) noexcept {
        if(!valid()) return false;
#ifdef CAPSTAN_UV_WORKER
        return executor_->try_dispatch(std::move(task));
#else
        executor_.dispatch(std::move(task));
        return true;
#endif
    }
};
// Lazily start the independent snapshot lane only when a read/Drain overlap
// needs it. Inline storage preserves the executor's over-alignment in C++14.
class snapshot_worker {
    typename std::aligned_storage<sizeof(business_worker),alignof(business_worker)>::type storage_;
    business_worker* worker_=nullptr;
public:
    snapshot_worker() = default;
    snapshot_worker(const snapshot_worker&)=delete;
    snapshot_worker& operator=(const snapshot_worker&)=delete;
    ~snapshot_worker() {close();}
    business_worker& get() {if(!worker_) worker_=new (&storage_) business_worker;return *worker_;}
    void close() noexcept {if(worker_) {worker_->~business_worker();worker_=nullptr;}}
};

}
