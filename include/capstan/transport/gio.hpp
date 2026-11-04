#pragma once
#include <capstan/transport/io.hpp>
#include <gio/gio.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
namespace capstan {
struct transport_stats {
    std::uint64_t sent_bytes=0, received_bytes=0, sent_frames=0, received_frames=0;
    std::uint64_t accepted_connections=0, rejected_connections=0, protocol_errors=0;
};
struct gio_limits {
    std::size_t max_frame=65536, max_connections=64, max_queued_frames=8;
};
// Wire: u32 big-endian length, u64 big-endian exchange sequence, payload.
// max_frame bounds payload bytes; sequence is echoed to fence duplicate replies.
// All construction/destruction/submit calls occur on the context owner thread
// (or before its loop starts). Server reply callbacks may run on any thread.
// Connection setup is explicitly synchronous; request/completion I/O is not.
class gio_client final : public io_endpoint {
    struct state;
    std::shared_ptr<state> state_;
    std::string setup_error_;
public:
    gio_client(GMainContext* context, const std::string& address, unsigned short port, gio_limits limits={});
    ~gio_client() override;
    bool valid() const noexcept;
    const std::string& setup_error() const noexcept {return setup_error_;}
    gio_client(const gio_client&)=delete;
    gio_client& operator=(const gio_client&)=delete;
    bool submit(bytes request, io_completion completion) noexcept override;
    void close() noexcept;
    transport_stats stats() const noexcept;
};
class gio_server {
    struct state;
    std::shared_ptr<state> state_;
    std::string setup_error_;
public:
    using handler = std::function<void(std::uint64_t connection_id, bytes request, io_completion reply)>;
    gio_server(GMainContext* context, unsigned short port, handler serve, gio_limits limits={});
    ~gio_server();
    bool valid() const noexcept;
    const std::string& setup_error() const noexcept {return setup_error_;}
    gio_server(const gio_server&)=delete;
    gio_server& operator=(const gio_server&)=delete;
    unsigned short port() const noexcept;
    void close() noexcept;
    transport_stats stats() const noexcept;
};
}
