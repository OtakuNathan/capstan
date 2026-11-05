#include <capstan/transport/gio.hpp>
#include "tests/check.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <vector>
#include <iostream>
using namespace capstan;
using clock_type=std::chrono::steady_clock;
#ifdef MSG_NOSIGNAL
constexpr int send_flags=MSG_NOSIGNAL;
#else
constexpr int send_flags=0;
#endif
void suppress_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    const int enabled=1;
    CHECK(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled))==0);
#else
    (void)fd;
#endif
}
void pump(GMainContext* context) { while(g_main_context_iteration(context,FALSE)) {} }
template<class F> void until(GMainContext* context,F done) {
    auto limit=clock_type::now()+std::chrono::seconds(4);
    while(!done()) {
        pump(context);
        CHECK(clock_type::now()<limit);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}
int raw_connect(unsigned short port) {
    int fd=socket(AF_INET,SOCK_STREAM,0); CHECK(fd>=0);
    suppress_sigpipe(fd);
    sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(port); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    CHECK(fcntl(fd,F_SETFL,O_NONBLOCK)==0);
    return fd;
}
bytes frame(bytes body,std::uint64_t sequence=1) {
    bytes b(12+body.size()); auto n=static_cast<std::uint32_t>(body.size()+8);
    for(unsigned i=0;i<4;++i) b[i]=static_cast<unsigned char>(n>>(24-8*i));
    for(unsigned i=0;i<8;++i) b[4+i]=static_cast<unsigned char>(sequence>>(56-8*i));
    std::copy(body.begin(),body.end(),b.begin()+12); return b;
}
void write_all(int fd,const bytes& data) {
    CHECK(send(fd,data.data(),data.size(),send_flags)==static_cast<ssize_t>(data.size()));
}
bytes receive_n(GMainContext* context,int fd,std::size_t n) {
    bytes result;
    until(context,[&] {
        unsigned char buffer[4096];
        auto got=recv(fd,buffer,std::min(sizeof(buffer),n-result.size()),0);
        if(got>0) result.insert(result.end(),buffer,buffer+got);
        return result.size()==n;
    });
    return result;
}
int main() {
    GMainContext* context=g_main_context_new();
    std::vector<io_completion> held;
    std::vector<std::thread> workers;
    std::vector<std::uint64_t> ids;
    {
        gio_limits limits; limits.max_queued_frames=3; limits.max_connections=8;
        gio_server server(context,0,[&](std::uint64_t id,bytes input,io_completion reply) {
            ids.push_back(id);
            if(input[0]==250) { held.push_back(std::move(reply)); return; }
            if(input[0]==252) { reply({outcome::completed,bytes(65536,42)}); return; }
            if(input[0]==251) {
                workers.emplace_back([reply,input]() mutable { reply({outcome::completed,std::move(input)}); }); return;
            }
            reply({outcome::completed,input});
            reply({outcome::completed,input}); // Duplicate guarded by stable state.
        },limits);
        unsigned replies=0;
        gio_client client(context,"127.0.0.1",server.port());
        CHECK(client.submit({1,2,3},[&](io_result r){CHECK(r.state==outcome::completed && r.payload==bytes({1,2,3})); ++replies;}));
        CHECK(!client.submit({9},[&](io_result){CHECK(false);}));
        until(context,[&]{return replies==1;});
        CHECK(client.submit({251,4,5},[&](io_result r){CHECK(r.payload==bytes({251,4,5})); ++replies;}));
        until(context,[&]{return replies==2;});
        for(auto& worker:workers) worker.join();
        workers.clear();
        CHECK(ids.size()==2 && ids[0]==ids[1]); // Persistent bound connection.
        CHECK(client.stats().sent_bytes==30 && client.stats().received_bytes==30);
        CHECK(client.stats().sent_frames==2 && client.stats().received_frames==2);
        CHECK(!client.submit(bytes(65537),[&](io_result){CHECK(false);}));
        // Partial header/body: no dispatch until the entire frame exists.
        int fd=raw_connect(server.port());
        auto fragmented=frame({3,7,8,9});
        auto initial=ids.size();
        for(std::size_t i=0;i<fragmented.size();++i) {
            CHECK(send(fd,&fragmented[i],1,send_flags)==1); pump(context);
            if(i+1<fragmented.size()) CHECK(ids.size()==initial);
        }
        CHECK(receive_n(context,fd,fragmented.size())==fragmented);
        // Two frames in one write, each dispatched/replied in order.
        auto first=frame({4,8},2); auto second=frame({5,9,10},3);
        bytes both=first; both.insert(both.end(),second.begin(),second.end());
        write_all(fd,both);
        CHECK(receive_n(context,fd,both.size())==both);
        close(fd); pump(context);
        // Oversized length is rejected before body allocation.
        int oversized=raw_connect(server.port());
        write_all(oversized,{0,1,0,9});
        until(context,[&]{return server.stats().protocol_errors>=1;});
        close(oversized);
        // Accepted request with peer disconnect completes unknown exactly once.
        CHECK(client.submit({250},[&](io_result r){CHECK(r.state==outcome::unknown); ++replies;}));
        until(context,[&]{return !held.empty();});
        client.close(); CHECK(replies==3);
        auto late=std::move(held.back()); held.pop_back();
        pump(context);
        late({outcome::completed,{8}}); pump(context); CHECK(replies==3);
        // A blocked application plus coalesced flood exceeds bounded queue.
        int slow=raw_connect(server.port());
        bytes flood;
        for(unsigned i=0;i<8;++i) { auto f=frame({250},i+1); flood.insert(flood.end(),f.begin(),f.end()); }
        write_all(slow,flood);
        until(context,[&]{return server.stats().protocol_errors>=2;});
        close(slow);
        for(auto& callback:held) callback({outcome::completed,{8}});
        held.clear(); pump(context);
        // A client which never reads responses cannot grow the write queue.
        int no_reader=raw_connect(server.port());
        auto before_pressure=server.stats().protocol_errors;
        for(unsigned i=0;i<100 && server.stats().protocol_errors==before_pressure;++i) {
            auto query=frame({252},i+1);
            auto sent=send(no_reader,query.data(),query.size(),send_flags);
            if(sent<0) break;
            CHECK(sent==static_cast<ssize_t>(query.size()));
            // Kernel delivery is asynchronous; allow the source to see each call.
            for(unsigned j=0;j<10;++j) { pump(context); std::this_thread::sleep_for(std::chrono::microseconds(100)); }
        }
        CHECK(server.stats().protocol_errors>before_pressure);
        close(no_reader); pump(context);
        // Connection cap rejects without installing an unbounded session.
        std::vector<int> connections;
        for(unsigned i=0;i<12;++i) { connections.push_back(raw_connect(server.port())); pump(context); }
        until(context,[&]{return server.stats().rejected_connections>=4;});
        for(int socket:connections) close(socket);
        pump(context);
        std::cout << "TCP partial header/body, coalesced frames, persistent identity, duplicate reply, worker completion, disconnect, bounded queues/sessions passed\n";
        server.close();
    }
    // The connector's five-second timeout must not expire an idle established
    // session. Keep dispatching sources throughout the idle interval.
    {
        gio_server server(context,0,[](std::uint64_t,bytes input,io_completion reply) {
            reply({outcome::completed,std::move(input)});
        });
        gio_client client(context,"127.0.0.1",server.port());
        unsigned replies=0;
        auto call=[&] {
            CHECK(client.submit({42},[&](io_result result) {
                CHECK(result.state==outcome::completed && result.payload==bytes({42}));
                ++replies;
            }));
        };
        call(); until(context,[&]{return replies==1;});
        auto* loop=g_main_loop_new(context,FALSE);
        auto* timer=g_timeout_source_new(6000);
        g_source_set_callback(timer,[](gpointer data)->gboolean {
            g_main_loop_quit(static_cast<GMainLoop*>(data)); return G_SOURCE_REMOVE;
        },loop,nullptr);
        g_source_attach(timer,context);
        g_main_loop_run(loop);
        g_source_unref(timer); g_main_loop_unref(loop);
        call(); until(context,[&]{return replies==2;});
        CHECK(client.stats().protocol_errors==0);
        std::cout << "TCP established session survives connector timeout interval\n";
    }
    // Duplicate wire replies must not be routed to a newly submitted exchange.
    // Use a deliberately faulty TCP peer; first completion submits request two
    // reentrantly while both copies of response one are already in the socket.
    {
        int listener=socket(AF_INET,SOCK_STREAM,0); CHECK(listener>=0);
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        CHECK(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
        CHECK(listen(listener,1)==0);
        socklen_t length=sizeof(address);
        CHECK(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length)==0);
        gio_client client(context,"127.0.0.1",ntohs(address.sin_port));
        int peer=accept(listener,nullptr,nullptr); CHECK(peer>=0);
        suppress_sigpipe(peer);
        CHECK(fcntl(peer,F_SETFL,O_NONBLOCK)==0);
        unsigned replies=0;
        CHECK(client.submit({1},[&](io_result r) {
            CHECK(r.state==outcome::completed && r.payload==bytes({41})); ++replies;
            CHECK(client.submit({2},[&](io_result second) {
                CHECK(second.state==outcome::completed && second.payload==bytes({42})); ++replies;
            }));
        }));
        CHECK(receive_n(context,peer,13)==frame({1},1));
        auto duplicate=frame({41},1); auto copy=duplicate;
        duplicate.insert(duplicate.end(),copy.begin(),copy.end());
        write_all(peer,duplicate);
        until(context,[&]{return replies==1;});
        CHECK(receive_n(context,peer,13)==frame({2},2));
        pump(context); CHECK(replies==1);
        write_all(peer,frame({42},2));
        until(context,[&]{return replies==2;});
        CHECK(client.submit({3},[&](io_result result) {CHECK(result.state==outcome::unknown); ++replies;}));
        CHECK(receive_n(context,peer,13)==frame({3},3));
        write_all(peer,frame({99},4)); // Future/unmatched response fails closed.
        until(context,[&]{return replies==3;});
        CHECK(client.stats().protocol_errors==1);
        close(peer); close(listener);
        std::cout << "TCP duplicate reply cannot complete reentrant next request; unmatched reply fails closed\n";
    }
    // Opt-in exchange window routes out-of-order replies and duplicates by
    // sequence, rejects excess submissions, and settles every accepted call.
    {
        int listener=socket(AF_INET,SOCK_STREAM,0);CHECK(listener>=0);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        CHECK(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);CHECK(listen(listener,1)==0);
        socklen_t length=sizeof(address);CHECK(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length)==0);
        gio_limits limits;limits.max_inflight=4;
        gio_client client(context,"127.0.0.1",ntohs(address.sin_port),limits);
        int peer=accept(listener,nullptr,nullptr);CHECK(peer>=0);suppress_sigpipe(peer);CHECK(fcntl(peer,F_SETFL,O_NONBLOCK)==0);
        unsigned replies=0,unknown=0;
        for(unsigned i=1;i<=4;++i) CHECK(client.submit({static_cast<unsigned char>(i)},[&,i](io_result r) {
            CHECK(r.state==outcome::completed && r.payload==bytes({static_cast<unsigned char>(40+i)}));++replies;
        }));
        CHECK(!client.submit({5},[](io_result) {CHECK(false);}));
        CHECK(receive_n(context,peer,52).size()==52);
        for(unsigned i=4;i>=1;--i) {write_all(peer,frame({static_cast<unsigned char>(40+i)},i));write_all(peer,frame({static_cast<unsigned char>(40+i)},i));}
        until(context,[&]{return replies==4;});CHECK(client.stats().protocol_errors==0);
        for(unsigned i=0;i<4;++i) CHECK(client.submit({9},[&](io_result r) {CHECK(r.state==outcome::unknown);++unknown;}));
        client.close();CHECK(unknown==4);pump(context);CHECK(replies==4 && unknown==4);
        close(peer);close(listener);
    }
    pump(context); g_main_context_unref(context);
}
