#include <capstan/error_policy.hpp>
#include <capstan/transport/gio.hpp>
#include <capstan/transport/context_dispatch.hpp>
#include <glib-unix.h>
#include <algorithm>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <array>
#include <atomic>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

// Uses the GSocket/GSource nonblocking pattern demonstrated in Dynabridge's
// benchmarks/rpc_call_benchmark.cpp (locked 99d363fc). This implementation is
// new Capstan framing/lifetime code; no upstream backend implementation copied.
namespace capstan {
namespace {
struct counters {
    std::atomic<std::uint64_t> sent_bytes{0},received_bytes{0},sent_frames{0},received_frames{0};
    std::atomic<std::uint64_t> accepted_connections{0},rejected_connections{0},protocol_errors{0};
    transport_stats snapshot() const noexcept {
        return {sent_bytes.load(),received_bytes.load(),sent_frames.load(),received_frames.load(),
            accepted_connections.load(),rejected_connections.load(),protocol_errors.load()};
    }
};
std::string socket_error(const char* where, GError* error) {
    std::string message=where;
    if(error) { message+=": "; message+=error->message; g_error_free(error); }
    return message;
}
bool blocked(GError* error) {
    return error && g_error_matches(error,G_IO_ERROR,G_IO_ERROR_WOULD_BLOCK);
}
void destroy_source(GSource*& source) noexcept {
    if(source) { auto* old=source; source=nullptr; g_source_destroy(old); g_source_unref(old); }
}
// Unlike invoke_full, an attached idle source never invokes on the submitting
// worker simply because the GMainContext is temporarily unowned.
void post(GMainContext* context,std::function<void()> action) {
    post_context(context,std::move(action));
}
struct channel : std::enable_shared_from_this<channel> {
    GMainContext* context;
    GSocket* socket;
    gio_limits limits;
    std::shared_ptr<counters> counts;
    GSource* read_source=nullptr;
    GSource* write_source=nullptr;
    bool closed=false;
    std::array<unsigned char,4> header{};
    std::size_t header_pos=0,body_pos=0;
    bytes body;
    struct outbound { bytes frame; std::size_t offset=0; };
    std::deque<outbound> writes;
    std::function<void(std::uint64_t,bytes)> on_frame;
    std::function<void()> on_close;
    channel(GMainContext* c,GSocket* s,gio_limits l,std::shared_ptr<counters> n)
        : context(g_main_context_ref(c)),socket(s),limits(l),counts(std::move(n)) {
        g_socket_set_blocking(socket,FALSE);
        // GSocketClient's connect timeout is inherited by the data socket.
        // Established exchanges rely on caller-installed explicit deadlines.
        g_socket_set_timeout(socket,0);
        g_socket_set_option(socket,IPPROTO_TCP,TCP_NODELAY,1,nullptr);
        // Bound kernel buffering as well as application queues. Linux may double
        // these values internally; neither grows with the number of requests.
        g_socket_set_option(socket,SOL_SOCKET,SO_SNDBUF,65536,nullptr);
        g_socket_set_option(socket,SOL_SOCKET,SO_RCVBUF,65536,nullptr);
    }
    ~channel() { close(); g_object_unref(socket); g_main_context_unref(context); }
    void attach() {
        // This Unix transport owns nonblocking sockets with no GSocket timeout
        // or cancellable. Watch their descriptors directly: GSocketSource's
        // timeout/readiness bookkeeping otherwise runs for every connection on
        // every context iteration, even when the socket has no event.
        read_source=g_unix_fd_source_new(g_socket_get_fd(socket),static_cast<GIOCondition>(G_IO_IN|G_IO_HUP|G_IO_ERR|G_IO_NVAL));
        auto* hold=new std::shared_ptr<channel>(shared_from_this());
        g_source_set_callback(read_source,G_SOURCE_FUNC(+[](gint,GIOCondition condition,gpointer p)->gboolean {
            auto self=*static_cast<std::shared_ptr<channel>*>(p);
            CAPSTAN_TRY {
                if(condition&(G_IO_ERR|G_IO_NVAL)) { self->close(); return G_SOURCE_REMOVE; }
                self->read();
            } CAPSTAN_CATCH_ALL { ++self->counts->protocol_errors; self->close(); }
            return self->closed ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
        }),hold,[](gpointer p){delete static_cast<std::shared_ptr<channel>*>(p);});
        g_source_attach(read_source,context);
    }
    void close() noexcept {
        if(closed) return;
        closed=true;
        destroy_source(read_source); destroy_source(write_source);
        g_socket_close(socket,nullptr);
        writes.clear(); body.clear();
        auto notify=std::move(on_close);
        on_frame={};
        if(notify) notify();
    }
    bool receive(unsigned char* data,std::size_t size,std::size_t& offset) {
        while(offset<size) {
            GError* error=nullptr;
            auto n=g_socket_receive(socket,reinterpret_cast<char*>(data)+offset,size-offset,nullptr,&error);
            if(n>0) { offset+=static_cast<std::size_t>(n); counts->received_bytes+=n; continue; }
            bool wait=n<0 && blocked(error);
            g_clear_error(&error);
            if(!wait) close();
            return false;
        }
        return true;
    }
    void read() {
        // Limit work per dispatch so one fast socket cannot monopolize admission.
        for(unsigned frames=0;frames<16 && !closed;++frames) {
            if(!receive(header.data(),header.size(),header_pos)) return;
            if(body.empty()) {
                std::size_t length=(std::size_t(header[0])<<24)|(std::size_t(header[1])<<16)|(std::size_t(header[2])<<8)|header[3];
                if(length<=8 || length>limits.max_frame+8) { ++counts->protocol_errors; close(); return; }
                body.resize(length);
            }
            if(!receive(body.data(),body.size(),body_pos)) return;
            std::uint64_t sequence=0;
            for(unsigned i=0;i<8;++i) sequence=(sequence<<8)|body[i];
            bytes frame(body.begin()+8,body.end());
            body.clear(); header_pos=body_pos=0;
            if(!sequence) { ++counts->protocol_errors; close(); return; }
            ++counts->received_frames;
            auto callback=on_frame;
            if(callback) callback(sequence,std::move(frame));
        }
    }
    bool send(bytes frame,std::uint64_t sequence) {
        if(closed || frame.empty() || frame.size()>limits.max_frame || writes.size()>=limits.max_queued_frames) return false;
        bytes framed(frame.size()+12);
        auto length=static_cast<std::uint32_t>(frame.size()+8);
        for(unsigned i=0;i<4;++i) framed[i]=static_cast<unsigned char>(length>>(24-8*i));
        for(unsigned i=0;i<8;++i) framed[4+i]=static_cast<unsigned char>(sequence>>(56-8*i));
        std::copy(frame.begin(),frame.end(),framed.begin()+12);
        writes.push_back({std::move(framed),0});
        flush();
        if(!closed && !writes.empty() && !write_source) {
            write_source=g_unix_fd_source_new(g_socket_get_fd(socket),static_cast<GIOCondition>(G_IO_OUT|G_IO_ERR|G_IO_HUP|G_IO_NVAL));
            auto* hold=new std::shared_ptr<channel>(shared_from_this());
            g_source_set_callback(write_source,G_SOURCE_FUNC(+[](gint,GIOCondition condition,gpointer p)->gboolean {
                auto self=*static_cast<std::shared_ptr<channel>*>(p);
                CAPSTAN_TRY {
                    if(condition&(G_IO_ERR|G_IO_NVAL|G_IO_HUP)) self->close();
                    else self->flush();
                    if(!self->closed && self->writes.empty()) destroy_source(self->write_source);
                } CAPSTAN_CATCH_ALL { self->close(); }
                return self->closed || self->writes.empty() ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
            }),hold,[](gpointer p){delete static_cast<std::shared_ptr<channel>*>(p);});
            g_source_attach(write_source,context);
        }
        return true; // Even an immediate socket failure is an accepted submission.
    }
    void flush() {
        while(!writes.empty() && !closed) {
            auto& out=writes.front();
            GError* error=nullptr;
            auto n=g_socket_send(socket,reinterpret_cast<const char*>(out.frame.data()+out.offset),out.frame.size()-out.offset,nullptr,&error);
            if(n>0) {
                out.offset+=static_cast<std::size_t>(n); counts->sent_bytes+=n;
                if(out.offset==out.frame.size()) { writes.pop_front(); ++counts->sent_frames; }
                continue;
            }
            bool wait=n<0 && blocked(error);
            g_clear_error(&error);
            if(!wait) close();
            return;
        }
    }
};
bool validate_limits(const gio_limits& limits) {
    if(!limits.max_frame || limits.max_frame>65536 || !limits.max_connections || limits.max_connections>1024 || !limits.max_queued_frames || limits.max_queued_frames>64)
        return false;
    return true;
}
}
struct gio_client::state : std::enable_shared_from_this<state> {
    std::shared_ptr<counters> counts=std::make_shared<counters>();
    std::shared_ptr<channel> wire;
    io_completion pending;
    std::uint64_t next_sequence=1,pending_sequence=0,completed_sequence=0;
    void finish(io_result result) noexcept {
        auto callback=std::move(pending);
        pending={};
        if(callback) completed_sequence=pending_sequence;
        if(callback) callback(std::move(result));
    }
};
gio_client::gio_client(GMainContext* context,const std::string& address,unsigned short port,gio_limits limits) {
    auto fail=[this](std::string error) {
        setup_error_=std::move(error);
#if CAPSTAN_HAS_EXCEPTIONS
        throw std::runtime_error(setup_error_);
#endif
    };
    if(!validate_limits(limits)) {fail("invalid bounded transport limits");return;}
    if(!context) context=g_main_context_default();
    // Setup is separate from submit: no blocking round_trip is hidden in FF.
    GInetAddress* numeric=g_inet_address_new_from_string(address.c_str());
    if(!numeric) {fail("transport requires an explicit numeric IP address");return;}
    GSocketAddress* target=g_inet_socket_address_new(numeric,port);
    g_object_unref(numeric);
    GSocketClient* connector=g_socket_client_new();
    g_socket_client_set_timeout(connector,5);
    // Trusted-lab connections must never consult desktop proxy settings or
    // route capability traffic through an environmental proxy.
    g_socket_client_set_enable_proxy(connector,FALSE);
    GError* error=nullptr;
    GSocketConnection* connection=g_socket_client_connect(connector,G_SOCKET_CONNECTABLE(target),nullptr,&error);
    g_object_unref(target);
    g_object_unref(connector);
    if(!connection) {fail(socket_error("connect",error));return;}
    GSocket* socket=g_socket_connection_get_socket(connection);
    g_object_ref(socket);
    g_object_unref(connection);
    state_=std::make_shared<state>();
    state_->wire=std::make_shared<channel>(context,socket,limits,state_->counts);
    std::weak_ptr<state> weak=state_;
    state_->wire->on_frame=[weak](std::uint64_t sequence,bytes frame) {
        if(auto s=weak.lock()) {
            // A stale duplicate is a different exchange, even if completion of
            // its original reply reentrantly submitted the next request.
            if(sequence<=s->completed_sequence) return;
            if(!s->pending || sequence!=s->pending_sequence) { ++s->counts->protocol_errors; s->wire->close(); return; }
            s->finish({outcome::completed,std::move(frame)});
        }
    };
    state_->wire->on_close=[weak] { if(auto s=weak.lock()) s->finish({outcome::unknown,{}}); };
    state_->wire->attach();
}
bool gio_client::valid() const noexcept {return setup_error_.empty() && state_ && state_->wire && !state_->wire->closed;}
gio_client::~gio_client() { close(); }
bool gio_client::submit(bytes request,io_completion completion) noexcept {
    auto s=state_;
    if(!s || s->wire->closed || s->pending || !completion || request.empty() || request.size()>s->wire->limits.max_frame || s->next_sequence==std::numeric_limits<std::uint64_t>::max()) return false;
    CAPSTAN_TRY {
        // All allocations in framing happen before any byte can be sent.
        s->pending=std::move(completion);
        s->pending_sequence=s->next_sequence++;
        if(!s->wire->send(std::move(request),s->pending_sequence)) { s->pending={}; return false; }
        return true;
    } CAPSTAN_CATCH_ALL {
        // A source-holder allocation can fail after bytes were sent. Retain
        // accepted-submit semantics and finish unknown exactly once.
        s->wire->close();
        return true;
    }
}
void gio_client::close() noexcept { if(state_ && state_->wire) state_->wire->close(); }
transport_stats gio_client::stats() const noexcept { return state_ ? state_->counts->snapshot() : transport_stats{}; }

struct gio_server::state : std::enable_shared_from_this<state> {
    struct session : std::enable_shared_from_this<session> {
        std::weak_ptr<state> server;
        std::uint64_t id;
        std::shared_ptr<channel> wire;
        std::deque<std::pair<std::uint64_t,bytes>> queue;
        std::uint64_t last_sequence=0;
        bool busy=false;
        void receive(std::uint64_t sequence,bytes frame) {
            if(sequence<=last_sequence) { ++wire->counts->protocol_errors; wire->close(); return; }
            last_sequence=sequence;
            if(queue.size()>=wire->limits.max_queued_frames) { ++wire->counts->protocol_errors; wire->close(); return; }
            queue.emplace_back(sequence,std::move(frame));
            next();
        }
        void next() {
            if(busy || queue.empty() || wire->closed) return;
            auto owner=server.lock();
            if(!owner || owner->closed) return;
            busy=true;
            auto sequence=queue.front().first;
            auto request=std::move(queue.front().second); queue.pop_front();
            auto once=std::make_shared<std::atomic<bool>>(false);
            std::weak_ptr<session> weak=shared_from_this();
            auto completion=[weak,once,sequence](io_result result) {
                if(once->exchange(true)) return; // Duplicate application completion.
                if(auto self=weak.lock()) {
                    post(self->wire->context,[weak,sequence,result=std::move(result)]() mutable {
                        auto self=weak.lock();
                        if(!self || self->wire->closed) return;
                        self->busy=false;
                        if(result.state!=outcome::completed) { self->wire->close(); return; }
                        if(!self->wire->send(std::move(result.payload),sequence)) { ++self->wire->counts->protocol_errors; self->wire->close(); return; }
                        self->next();
                    });
                }
            };
            CAPSTAN_TRY { owner->serve(id,std::move(request),completion); }
            CAPSTAN_CATCH_ALL { completion({outcome::unknown,{}}); }
        }
    };
    GMainContext* context;
    GSocket* listener=nullptr;
    GSource* accept_source=nullptr;
    gio_limits limits;
    handler serve;
    std::shared_ptr<counters> counts=std::make_shared<counters>();
    std::map<std::uint64_t,std::shared_ptr<session>> sessions;
    std::uint64_t next_id=1;
    unsigned short bound_port=0;
    bool closed=false;
    state(GMainContext* c,gio_limits l,handler h) : context(g_main_context_ref(c)),limits(l),serve(std::move(h)) {}
    ~state() { close(); if(listener) g_object_unref(listener); g_main_context_unref(context); }
    void close() noexcept {
        if(closed) return;
        closed=true;
        destroy_source(accept_source);
        if(listener) g_socket_close(listener,nullptr);
        // on_close erases from sessions. Detach ownership first.
        auto old=std::move(sessions);
        for(auto& item:old) item.second->wire->close();
    }
    void accept() {
        for(unsigned i=0;i<16 && !closed;++i) {
            GError* error=nullptr;
            GSocket* socket=g_socket_accept(listener,nullptr,&error);
            if(!socket) { bool wait=blocked(error); g_clear_error(&error); if(!wait) close(); return; }
            if(sessions.size()>=limits.max_connections || next_id==std::numeric_limits<std::uint64_t>::max()) {
                ++counts->rejected_connections; g_socket_close(socket,nullptr); g_object_unref(socket); continue;
            }
            auto client=std::make_shared<session>();
            client->id=next_id++; client->server=shared_from_this();
            client->wire=std::make_shared<channel>(context,socket,limits,counts);
            std::weak_ptr<session> weak=client;
            std::weak_ptr<state> owner=shared_from_this();
            const auto id=client->id;
            client->wire->on_frame=[weak](std::uint64_t sequence,bytes frame) { if(auto s=weak.lock()) s->receive(sequence,std::move(frame)); };
            client->wire->on_close=[owner,id] { if(auto s=owner.lock()) s->sessions.erase(id); };
            sessions.emplace(id,client);
            ++counts->accepted_connections;
            client->wire->attach();
        }
    }
};
gio_server::gio_server(GMainContext* context,unsigned short port,handler serve,gio_limits limits) {
    auto fail=[this](std::string error) {
        setup_error_=std::move(error);
#if CAPSTAN_HAS_EXCEPTIONS
        throw std::runtime_error(setup_error_);
#endif
    };
    if(!validate_limits(limits)) {fail("invalid bounded transport limits");return;}
    if(!serve) {fail("server handler required");return;}
    if(!context) context=g_main_context_default();
    state_=std::make_shared<state>(context,limits,std::move(serve));
    GError* error=nullptr;
    state_->listener=g_socket_new(G_SOCKET_FAMILY_IPV4,G_SOCKET_TYPE_STREAM,G_SOCKET_PROTOCOL_TCP,&error);
    if(!state_->listener) {fail(socket_error("socket",error));return;}
    g_socket_set_blocking(state_->listener,FALSE);
    GInetAddress* loopback=g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress* address=g_inet_socket_address_new(loopback,port);
    g_object_unref(loopback);
    bool bound=g_socket_bind(state_->listener,address,TRUE,&error);
    g_object_unref(address);
    if(!bound) {fail(socket_error("bind",error));return;}
    if(!g_socket_listen(state_->listener,&error)) {fail(socket_error("listen",error));return;}
    address=g_socket_get_local_address(state_->listener,&error);
    if(!address) {fail(socket_error("local address",error));return;}
    state_->bound_port=g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(address));
    g_object_unref(address);
    state_->accept_source=g_unix_fd_source_new(g_socket_get_fd(state_->listener),static_cast<GIOCondition>(G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL));
    auto* hold=new std::shared_ptr<state>(state_);
    g_source_set_callback(state_->accept_source,G_SOURCE_FUNC(+[](gint,GIOCondition condition,gpointer p)->gboolean {
        auto self=*static_cast<std::shared_ptr<state>*>(p);
        CAPSTAN_TRY { if(condition&(G_IO_ERR|G_IO_HUP|G_IO_NVAL)) self->close(); else self->accept(); }
        CAPSTAN_CATCH_ALL { self->close(); }
        return self->closed ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
    }),hold,[](gpointer p){delete static_cast<std::shared_ptr<state>*>(p);});
    g_source_attach(state_->accept_source,context);
}
bool gio_server::valid() const noexcept {return setup_error_.empty() && state_ && state_->bound_port && !state_->closed;}
gio_server::~gio_server() { close(); }
unsigned short gio_server::port() const noexcept { return state_ ? state_->bound_port : 0; }
void gio_server::close() noexcept { if(state_) state_->close(); }
transport_stats gio_server::stats() const noexcept { return state_ ? state_->counts->snapshot() : transport_stats{}; }
}
