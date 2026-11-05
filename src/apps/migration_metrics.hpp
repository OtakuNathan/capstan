#pragma once
#include <chrono>
#include <ctime>
#include <iostream>
#include <cstdint>
#include <capstan/migration/gio_digest.hpp>
namespace app {
// Diagnostic only: durations use one process's monotonic clock. CPU counters
// distinguish owner/worker thread execution from aggregate process execution.
struct metric_stamp {
    std::uint64_t wall=0,cpu=0,process_cpu=0;
    static metric_stamp now(bool enabled=true) noexcept {
        if(!enabled) return {};
        timespec thread{},process{};
        if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&thread) || clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&process)) std::terminate();
        return {static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()),
                static_cast<std::uint64_t>(thread.tv_sec)*1000000000+static_cast<std::uint64_t>(thread.tv_nsec),
                static_cast<std::uint64_t>(process.tv_sec)*1000000000+static_cast<std::uint64_t>(process.tv_nsec)};
    }
};
inline void emit_metric(const char* name,metric_stamp start,metric_stamp end) {
    if(!start.wall) return;
    std::cout<<"PROFILE name="<<name<<" wall_ns="<<end.wall-start.wall<<" cpu_ns="<<end.cpu-start.cpu
             <<" process_cpu_ns="<<end.process_cpu-start.process_cpu<<'\n'<<std::flush;
}
struct metric_total {
    std::uint64_t wall=0,cpu=0,process_cpu=0;
    void add(metric_stamp start,metric_stamp end) noexcept {wall+=end.wall-start.wall;cpu+=end.cpu-start.cpu;process_cpu+=end.process_cpu-start.process_cpu;}
    void emit(const char* name) const {std::cout<<"PROFILE name="<<name<<" wall_ns="<<wall<<" cpu_ns="<<cpu<<" process_cpu_ns="<<process_cpu<<'\n'<<std::flush;}
};
struct transfer_metrics {metric_total digest,decode,append;};
class measured_digest final : public capstan::migration::transfer_digest {
    std::unique_ptr<capstan::migration::transfer_digest> digest_=capstan::migration::make_sha256_stream();
    std::shared_ptr<transfer_metrics> metrics_;
public:
    explicit measured_digest(std::shared_ptr<transfer_metrics> metrics):metrics_(std::move(metrics)) {}
    void update(const std::uint8_t* data,std::size_t size) noexcept override {
        const auto started=metric_stamp::now();digest_->update(data,size);metrics_->digest.add(started,metric_stamp::now());
    }
    std::array<std::uint8_t,32> value() noexcept override {
        const auto started=metric_stamp::now();auto result=digest_->value();metrics_->digest.add(started,metric_stamp::now());return result;
    }
};

}
