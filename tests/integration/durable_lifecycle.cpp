#include "examples/owner/sqlite.hpp"
#include "src/apps/owner_host.hpp"
#include <capstan/async/detachable.hpp>
#include "tests/check.hpp"
#include <future>
#include <cstdio>
#include <dirent.h>
#include <fstream>
using backend_type=examples::sqlite_owner_backend;
using runtime_type=basic_host_runtime<backend_type>;
namespace {
app::config config() {app::config c{{1,2,3},4,{}};for(unsigned i=0;i<3;++i)c.credentials[i][0]=10+i;return c;}
grant initial() {grant g;g.key=config().key;g.epoch=1;g.holder=1;g.grant_id=11;g.mode=access_mode::exclusive;g.secret[0]=21;return g;}
grant successor() {auto g=initial();g.epoch=2;g.holder=2;g.grant_id=12;g.secret[0]=22;return g;}
std::string temporary() {char path[]="/tmp/capstan-life-XXXXXX";int fd=mkstemp(path);CHECK(fd>=0);close(fd);return path;}
void remove_files(const std::string& p) {for(auto suffix:{"","-wal","-shm",".host.lock"})std::remove((p+suffix).c_str());}
bytes auth(unsigned role,bytes call) {return wire::command(9999,{wire::value::string(app::token_bytes(config().credentials[role])),wire::value::string(wire::binary(call))});}
bytes write(id request=1) {return wire::encode_call({initial(),request,method::add_range},{wire::u(0),wire::u(1),wire::u(7)});}
void pump(GMainContext* c,const std::function<bool()>& done) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!done()) {while(g_main_context_iteration(c,FALSE)) {} CHECK(std::chrono::steady_clock::now()<end);std::this_thread::sleep_for(std::chrono::microseconds(100));}
}
wire::reply send(runtime_type& host,GMainContext* c,unsigned role,bytes call) {
    bool done=false;wire::reply result;
    host.handle(0,auth(role,std::move(call)),[&](io_result io){CHECK(io.state==outcome::completed);CHECK(wire::try_decode_reply(io.payload,result));done=true;});
    pump(c,[&]{return done;});return result;
}
void install(runtime_type& h,GMainContext* c) {
    transition t{config().key,config().control,1,0,1,false,{initial()}};
    CHECK(send(h,c,2,wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition(t)))})).code==status::ok);
}
void bootstrap(const std::string& path) {auto* c=g_main_context_new();{auto b=std::make_shared<backend_type>(path);runtime_type h(config(),c,1024,0,1024,b);h.trace_requests=false;install(h,c);}g_main_context_unref(c);}
std::size_t entries(const char* name) {
    auto* d=opendir(name);if(!d)return 0;std::size_t n=0;
    while(auto* e=readdir(d)) if(e->d_name[0]!='.')++n;
    closedir(d);return n;
}
// pthread_join synchronizes C++ thread completion, but Linux may retain the
// exiting task in /proc briefly. Do not mistake that observation window for a
// leaked live worker. Keep a deadline so a genuinely retained thread still fails.
bool threads_settled(std::size_t limit,std::chrono::milliseconds timeout) {
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    do {
        if(entries("/proc/self/task")<=limit)return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now()<deadline);
    return entries("/proc/self/task")<=limit;
}
void dump_threads(unsigned cycle,std::size_t baseline) {
    std::cerr<<"thread teardown deadline exceeded: cycle="<<cycle
             <<" baseline="<<baseline<<" observed="<<entries("/proc/self/task")<<'\n';
    auto* d=opendir("/proc/self/task");if(!d)return;
    while(auto* e=readdir(d)) if(e->d_name[0]!='.') {
        std::ifstream f(std::string("/proc/self/task/")+e->d_name+"/status");
        std::string line;std::cerr<<"tid="<<e->d_name<<'\n';
        while(std::getline(f,line))if(line.compare(0,5,"Name:")==0||line.compare(0,6,"State:")==0)std::cerr<<line<<'\n';
    }
    closedir(d);
}
void check_thread_observer() {
    const auto baseline=entries("/proc/self/task");if(!baseline)return;
    std::promise<void> entered,release;auto ready=entered.get_future();auto gate=release.get_future();
    std::thread held([&]{entered.set_value();gate.wait();});
    ready.wait();
    // Negative control: a live retained thread must not pass bounded settling.
    const bool falsely_settled=threads_settled(baseline,std::chrono::milliseconds(20));
    release.set_value();held.join();
    CHECK(!falsely_settled);
    CHECK(threads_settled(baseline,std::chrono::milliseconds(1000)));
}
struct local_endpoint final:io_endpoint {
    runtime_type& host;unsigned completions=0;
    explicit local_endpoint(runtime_type& h):host(h) {}
    bool submit(bytes request,io_completion done) noexcept override {
        host.handle(0,auth(0,std::move(request)),[this,done=std::move(done)](io_result io) mutable {++completions;done(std::move(io));});
        return true;
    }
};
}
int main() {
    // The demo Host has process lifetime. Test graceful teardown only after
    // native work and its owner-loop continuations have truly drained.
    check_thread_observer();
    auto path=temporary();std::size_t baseline_fds=0;
    const auto baseline_threads=entries("/proc/self/task");
    id version=0;bytes expected;
    for(unsigned cycle=0;cycle<32;++cycle) {
        auto* c=g_main_context_new();std::weak_ptr<backend_type> weak;
        {
            auto b=std::make_shared<backend_type>(path);weak=b;
            runtime_type h(config(),c,1024,0,1024,b);b.reset();h.trace_requests=false;
            if(!cycle)install(h,c);
            bytes image;CHECK(h.owner->capture_snapshot(image)==status::ok);
            if(cycle)CHECK(image==expected);
            for(unsigned n=0;n<4;++n) {
                const id request=1+cycle*8+n;
                auto r=send(h,c,0,write(request));CHECK(r.code==status::ok&&wire::number(r.values.at(0))==++version);
                auto again=send(h,c,0,write(request));CHECK(again.code==status::ok&&wire::number(again.values.at(0))==version);
            }
            auto r=send(h,c,0,wire::encode_call({initial(),5+cycle*8,method::version}));CHECK(r.code==status::ok&&wire::number(r.values.at(0))==version);
            CHECK(h.owner->capture_snapshot(expected)==status::ok);
            CHECK(h.authority.inspect().active==0&&h.operations.empty()&&h.admitted==h.completed);
            while(g_main_context_iteration(c,FALSE)) {}
        }
        CHECK(weak.expired()); // No posted continuation keeps the backend alive.
        while(g_main_context_iteration(c,FALSE)) {}g_main_context_unref(c);
        const auto fds=entries("/proc/self/fd");
        if(cycle==1)baseline_fds=fds;
        if(cycle>1) {
            if(baseline_fds)CHECK(fds<=baseline_fds);
            if(baseline_threads) {
                const bool settled=threads_settled(baseline_threads,std::chrono::milliseconds(1000));
                if(!settled)dump_threads(cycle,baseline_threads);
                CHECK(settled);
            }
        }
    }
    remove_files(path);
    // Timeout detaches only the observer. Destroying its wrapper must neither
    // cancel server work nor free its retained lifetime before real completion.
    for(unsigned cycle=0;cycle<8;++cycle) {
        auto file=temporary();auto* c=g_main_context_new();
        {
            auto b=std::make_shared<backend_type>(file);runtime_type h(config(),c,1024,0,128,b);h.trace_requests=false;install(h,c);
            local_endpoint endpoint(h);std::promise<void> entered,release;auto ready=entered.get_future();auto gate=release.get_future().share();
            CHECK(h.worker.try_dispatch(flux_foundry::task_wrapper_sbo([&entered,gate]() noexcept {entered.set_value();gate.wait();})));
            CHECK(ready.wait_for(std::chrono::seconds(5))==std::future_status::ready);
            unsigned observations=0,acks=0;auto lifetime=std::make_shared<int>(7);std::weak_ptr<int> weak=lifetime;
            {
                detachable_endpoint timed(endpoint);
                CHECK(timed.submit_owned(write(),[&](io_result io){CHECK(io.state==outcome::unknown);++observations;},lifetime));
                lifetime.reset();CHECK(!weak.expired());
                CHECK(timed.timeout_pending());CHECK(!timed.timeout_pending());CHECK(observations==1&&!weak.expired());
            }
            CHECK(h.authority.inspect().active==1&&!weak.expired());
            transition t{config().key,config().control,2,1,2,false,{successor()}};
            h.handle(0,auth(2,wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition(t)))})),[&](io_result io){CHECK(wire::decode_reply(io.payload).code==status::ok&&h.authority.inspect().active==0);++acks;});
            while(g_main_context_iteration(c,FALSE)) {}
            CHECK(!acks&&!endpoint.completions&&h.authority.inspect().epoch==1);
            release.set_value();pump(c,[&]{return acks==1&&endpoint.completions==1;});
            CHECK(observations==1&&weak.expired()&&h.authority.inspect().epoch==2);
            CHECK(h.operations.empty()&&h.admitted==h.completed);
            auto old=send(h,c,0,write());CHECK(old.code==status::ok&&wire::number(old.values.at(0))==1);
            auto r=send(h,c,1,wire::encode_call({successor(),2,method::version}));CHECK(r.code==status::ok&&wire::number(r.values.at(0))==1);
        }
        while(g_main_context_iteration(c,FALSE)) {}g_main_context_unref(c);remove_files(file);
    }
    // Real loopback disconnect: closing the socket settles its caller Unknown,
    // but does not revoke an admitted Host operation or roll back its effect.
    for(unsigned cycle=0;cycle<8;++cycle) {
        auto file=temporary();auto* c=g_main_context_new();
        {
            auto b=std::make_shared<backend_type>(file);runtime_type h(config(),c,1024,0,128,b);h.trace_requests=false;install(h,c);
            gio_server server(c,0,[&](id connection,bytes packet,io_completion done){h.handle(connection,std::move(packet),std::move(done));});CHECK(server.valid());
            std::promise<void> entered,release;auto ready=entered.get_future();auto gate=release.get_future().share();
            CHECK(h.worker.try_dispatch(flux_foundry::task_wrapper_sbo([&entered,gate]() noexcept {entered.set_value();gate.wait();})));
            CHECK(ready.wait_for(std::chrono::seconds(5))==std::future_status::ready);
            unsigned callbacks=0;
            {
                gio_client client(c,"127.0.0.1",server.port());CHECK(client.valid());
                CHECK(client.submit(auth(0,write()),[&](io_result io){CHECK(io.state==outcome::unknown);++callbacks;}));
                pump(c,[&]{return h.admitted==1;});CHECK(h.authority.inspect().active==1);
                client.close();CHECK(callbacks==1&&h.authority.inspect().active==1);
            }
            release.set_value();pump(c,[&]{return h.completed==1;});
            CHECK(callbacks==1&&h.authority.inspect().active==0&&h.operations.empty());
            {
                gio_client retry(c,"127.0.0.1",server.port());CHECK(retry.valid());bool received=false;
                CHECK(retry.submit(auth(0,write()),[&](io_result io){CHECK(io.state==outcome::completed);auto r=wire::decode_reply(io.payload);CHECK(r.code==status::ok&&wire::number(r.values.at(0))==1);received=true;}));
                pump(c,[&]{return received;});retry.close();
            }
            auto r=send(h,c,0,wire::encode_call({initial(),2,method::version}));CHECK(r.code==status::ok&&wire::number(r.values.at(0))==1);
            server.close();while(g_main_context_iteration(c,FALSE)) {}
        }
        while(g_main_context_iteration(c,FALSE)) {}g_main_context_unref(c);remove_files(file);
    }
    // Crash after in-place publication and posting the owner-loop completion,
    // but before any reply/pin retirement. This is a different cut from the
    // existing after-COMMIT hook, which runs before publication.
    for(unsigned cycle=0;cycle<8;++cycle) {
        auto file=temporary();bootstrap(file);auto child=fork();CHECK(child>=0);
        if(child==0) {
            auto* c=g_main_context_new();auto b=std::make_shared<backend_type>(file);runtime_type h(config(),c,1024,0,1024,b);h.trace_requests=false;
            h.handle(0,auth(0,write()),[](io_result){_exit(91);});
            std::promise<void> published;auto ready=published.get_future();
            CHECK(h.worker.try_dispatch(flux_foundry::task_wrapper_sbo([&published]() noexcept {published.set_value();})));
            CHECK(ready.wait_for(std::chrono::seconds(5))==std::future_status::ready);
            CHECK(h.authority.inspect().active==1); // No owner-loop pump yet.
            _exit(87);
        }
        int state;CHECK(waitpid(child,&state,0)==child&&WIFEXITED(state)&&WEXITSTATUS(state)==87);
        auto* c=g_main_context_new();
        {auto b=std::make_shared<backend_type>(file);runtime_type h(config(),c,1024,0,1024,b);h.trace_requests=false;
            CHECK(h.authority.inspect().active==0);
            auto r=send(h,c,0,write());CHECK(r.code==status::ok&&wire::number(r.values.at(0))==1);
            r=send(h,c,0,write(2));CHECK(r.code==status::ok&&wire::number(r.values.at(0))==2);
            CHECK(h.authority.inspect().active==0&&h.operations.empty());
        }
        while(g_main_context_iteration(c,FALSE)) {}g_main_context_unref(c);remove_files(file);
    }
    std::cout<<"32 graceful reopen cycles, 8 detach/Close/Drain lifetime cycles, 8 loopback disconnect/retry cycles, 8 post-publication crashes passed; fd/thread baseline "<<baseline_fds<<'/'<<baseline_threads<<'\n';
}
