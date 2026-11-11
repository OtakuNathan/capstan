#include <capstan/error_policy.hpp>
#include "common.hpp"
#include <capstan/access/transfer.hpp>
#include <capstan/async/detachable.hpp>
#include <glib-unix.h>
#include <unistd.h>
#include <fcntl.h>
#include <chrono>
#include <deque>
using namespace capstan;
struct client_runtime {
    GMainLoop* loop;
    gio_client host_socket,table_socket,control_socket;
    app::authenticated_endpoint host,table,control;
    bool control_busy=false;
    std::shared_ptr<transfer_plan> last_transfer;
    detachable_endpoint timed_host;
    GSource* deadline=nullptr;
    std::unique_ptr<read_capability> read;
    std::unique_ptr<exclusive_capability> write;
    grant current{},old{};
    bool have=false,have_old=false,busy=false,eof=false;
    id control_request=1,raw_request=(id{1}<<63);
    std::deque<std::string> commands;
    std::string input,label;
    std::chrono::steady_clock::time_point started;
    client_runtime(GMainLoop* l,unsigned short hp,unsigned short tp,token credential)
        :loop(l),host_socket(g_main_loop_get_context(l),"127.0.0.1",hp),table_socket(g_main_loop_get_context(l),"127.0.0.1",tp),control_socket(g_main_loop_get_context(l),"127.0.0.1",hp),host(host_socket,credential),table(table_socket,credential),control(control_socket,credential),timed_host(host){}
    ~client_runtime() { if(deadline) {g_source_destroy(deadline);g_source_unref(deadline);} }
    void finish(wire::reply r) {
        auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
        std::cout<<"RESULT command="<<label<<" status="<<status_name(r.code)<<" outcome="<<static_cast<unsigned>(r.state)<<" ns="<<ns;
        for(std::size_t i=0;i<r.values.size();++i) if(r.values[i].kind()==portal::rpc::value_kind::unsigned_integer) std::cout<<" v"<<i<<'='<<wire::number(r.values[i]);
        std::cout<<'\n'<<std::flush;busy=false;
        app::post(g_main_loop_get_context(loop),[this]{pump();});
    }
    void finish(io_result io) {
        if(io.state!=outcome::completed) {finish({io.state,status::invalid,{}});return;}
        wire::reply reply;if(!wire::try_decode_reply(io.payload,reply)) {finish({outcome::unknown,status::invalid,{}});return;} finish(std::move(reply));
    }
    void fetch() {
        start_io(table,wire::command(wire::fetch_grant),[this](io_result io) {
            CAPSTAN_TRY {
                wire::reply r;if(io.state!=outcome::completed || !wire::try_decode_reply(io.payload,r)) {finish({outcome::unknown,status::invalid,{}});return;}
                if(r.code==status::ok) {
                    grant g;if(r.values.size()!=1 || !wire::is_string(r.values[0]) || !wire::try_decode_grant(wire::string(r.values[0]),g)) {finish({outcome::unknown,status::invalid,{}});return;}
                    read.reset();write.reset();have=false;
                    if(g.mode==access_mode::shared) read.reset(new read_capability(g));
                    else {status code;write=exclusive_capability::try_create(g,code);if(!write) {finish({outcome::unknown,code,{}});return;}}
                    current=g;have=true;r.values={wire::u(g.epoch),wire::u(static_cast<unsigned>(g.mode))};
                }
                finish(std::move(r));
            } CAPSTAN_CATCH_ALL {finish({outcome::unknown,status::invalid,{}});}
        });
    }
    // This connection can progress while a business RPC retains its Lease guard.
    void direct(std::string command) {
        std::istringstream words(command);std::string verb;words>>verb;
        const auto begin=std::chrono::steady_clock::now();control_busy=true;
        auto complete=[this,verb,begin](wire::reply r) {
            auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
            std::cout<<"RESULT command="<<verb<<" status="<<status_name(r.code)<<" outcome="<<static_cast<unsigned>(r.state)<<" ns="<<ns;
            for(std::size_t i=0;i<r.values.size();++i) if(wire::is_number(r.values[i])) std::cout<<" v"<<i<<'='<<wire::number(r.values[i]);
            std::cout<<'\n'<<std::flush;control_busy=false;
            app::post(g_main_loop_get_context(loop),[this]{pump();});
        };
        CAPSTAN_TRY {
            auto decode=[complete](io_result io) mutable {
                wire::reply r{io.state,status::invalid,{}};
                if(io.state==outcome::completed && !wire::try_decode_reply(io.payload,r)) r={outcome::unknown,status::invalid,{}};
                if((r.code==status::ok || r.code==status::pending || r.code==status::not_seen) && r.values.size()==1 && wire::is_string(r.values[0])) {
                    transfer_receipt receipt;
                    if(!wire::try_decode_transfer_receipt(wire::string(r.values[0]),receipt)) {complete({outcome::unknown,status::invalid,{}});return;}
                    const auto& ref=receipt.reference;
                    r.values={wire::u(ref.key.cluster),wire::u(ref.key.incarnation),wire::u(ref.key.object),wire::u(ref.from_holder),wire::u(ref.to_holder),wire::u(ref.source_epoch),wire::u(ref.transaction.sequence),wire::u(receipt.installed_epoch),wire::u(static_cast<unsigned>(receipt.usability)),wire::u(static_cast<unsigned>(receipt.observation))};
                }
                complete(std::move(r));
            };
            if(verb=="transfer") {
                id recipient=0,sequence=0;words>>recipient>>sequence;status code=status::invalid;
                if(!words || !have || !write) {complete({outcome::not_submitted,code,{}});return;}
                auto prepared=transfer_plan::prepare(*write,recipient,sequence,code);
                if(!prepared) {complete({outcome::not_submitted,code,{}});return;}
                last_transfer=std::shared_ptr<transfer_plan>(std::move(prepared));auto plan=last_transfer;
                plan->start(control,[this,plan,decode](io_result io) mutable {
                    if(plan->sealed()) {have=false;read.reset();write.reset();}
                    decode(std::move(io));
                });return;
            }
            if(verb=="retry_transfer") {
                if(!last_transfer) {complete({outcome::not_submitted,status::invalid,{}});return;}
                last_transfer->retry(control,std::move(decode));return;
            }
            transfer_ref ref;words>>ref.key.cluster>>ref.key.incarnation>>ref.key.object>>ref.from_holder>>ref.to_holder>>ref.source_epoch>>ref.transaction.sequence;
            ref.transaction.origin=authority_origin::holder_transfer;ref.transaction.issuer=ref.from_holder;
            if(!words) {complete({outcome::not_submitted,status::invalid,{}});return;}
            if(verb!="claim") {
                auto packet=wire::command(wire::query_transfer,{wire::value::string(wire::encode_transfer_ref(ref))});
                start_io(control,std::move(packet),std::move(decode));return;
            }
            if(busy) {complete({outcome::not_submitted,status::busy,{}});return;}
            capstan::claim_transfer(control,ref,[this,complete](transfer_claim_result result) mutable {
                wire::reply reply{result.state,result.code,{}};
                if(result.capability) {
                    read.reset();write=std::move(result.capability);current=result.metadata;have=true;
                    reply.values={wire::u(current.epoch),wire::u(static_cast<unsigned>(current.mode))};
                }
                complete(std::move(reply));
            });
        } CAPSTAN_CATCH_ALL {complete({outcome::unknown,status::invalid,{}});}
    }
    void bad_input() {finish({outcome::not_submitted,status::invalid,{}});}
    void pump() {
        if(control_busy) return;
        if(!commands.empty()) {
            auto verb=commands.front().substr(0,commands.front().find(' '));
            if(verb=="transfer" || verb=="retry_transfer" || verb=="query_transfer" || verb=="claim") {
                auto command=std::move(commands.front());commands.pop_front();direct(std::move(command));return;
            }
        }
        if(busy) return;
        if(commands.empty()) {if(eof) g_main_loop_quit(loop);return;}
        auto command=commands.front();commands.pop_front(); std::istringstream words(command);words>>label;
        busy=true;started=std::chrono::steady_clock::now();
        CAPSTAN_TRY {
            auto done=[this](wire::reply r){finish(std::move(r));};
            if(label=="quit") {busy=false;g_main_loop_quit(loop);return;}
            if(label=="fetch") {fetch();return;}
            if(label=="reconcile" || label=="retry_admin") {
                start_io(table,wire::command(2003,{wire::value::boolean(label=="retry_admin")}),[this](io_result io){finish(std::move(io));});return;
            }
            if(label=="keep") {if(!(have)) {bad_input();return;}old=current;have_old=true;finish({outcome::completed,status::ok,{}});return;}
            if(label=="shared" || label=="exclusive" || label=="destroy") {
                std::vector<wire::value> a{wire::u(control_request++),wire::value::boolean(label=="destroy")};
                if(label=="shared") {a.push_back(wire::u(1));a.push_back(wire::u(1));a.push_back(wire::u(2));a.push_back(wire::u(1));}
                if(label=="exclusive") {id holder=0;words>>holder;if(!(holder==1||holder==2)) {bad_input();return;}a.push_back(wire::u(holder));a.push_back(wire::u(2));}
                start_io(table,wire::command(wire::configure_grants,a),[this](io_result io){finish(std::move(io));});return;
            }
            if(label=="release_old") {
                if(!have_old) {bad_input();return;}
                start_io(table,wire::command(2002,{wire::u(control_request++),wire::value::string(wire::encode_grant(old))}),[this](io_result io){finish(std::move(io));});return;
            }
            if(label=="close") {
                if(!(have)) {bad_input();return;}
                auto request=wire::command(2002,{wire::u(control_request++),wire::value::string(wire::encode_grant(current))});
                auto cb=[this](io_result io){have=false;read.reset();write.reset();finish(std::move(io));};
                bool accepted=read ? read->try_close(table,std::move(request),std::move(cb)) : write->try_close(table,std::move(request),std::move(cb));if(!accepted) bad_input();return;
            }
            if(label=="version") {
                if(!(have)) {bad_input();return;}
                auto call=read ? read->try_version() : write->try_version();if(!call) {bad_input();return;}
                start_call(host,std::move(*call),std::function<void(wire::reply)>(done));
                return;
            }
            if(label=="summary") {
                if(!(have)) {bad_input();return;}
                auto cb=std::function<void(wire::reply)>([this](wire::reply r){
                    if(r.code==status::ok) {
                        summary_t s;if(!typed_call<summary_t>::try_decode(r,s)) {finish({outcome::unknown,status::invalid,{}});return;}
                        r.values={wire::u(s.count),wire::u(s.sum),wire::u(s.checksum),wire::u(s.version)};
                    }
                    finish(std::move(r));
                });
                auto call=read ? read->try_summary() : write->try_summary();if(!call) {bad_input();return;}
                start_call(host,std::move(*call),std::move(cb));return;
            }
            if(label=="timeout_add") {
                id begin,end,delta,ms;words>>begin>>end>>delta>>ms;if(!(words && write && ms>0 && ms<=1000)) {bad_input();return;}
                auto invocation=write->try_add_range(begin,end,delta);if(!invocation) {bad_input();return;}
                deadline=g_timeout_source_new(static_cast<unsigned>(ms));
                g_source_set_callback(deadline,[](gpointer p)->gboolean {
                    auto* c=static_cast<client_runtime*>(p);
                    auto* timer=c->deadline;c->deadline=nullptr;g_source_unref(timer);
                    c->timed_host.timeout_pending();return G_SOURCE_REMOVE;
                },this,nullptr);
                g_source_attach(deadline,g_main_loop_get_context(loop));
                start_call(timed_host,std::move(*invocation),std::function<void(wire::reply)>([this](wire::reply r){
                    if(deadline) {g_source_destroy(deadline);g_source_unref(deadline);deadline=nullptr;}
                    finish(std::move(r));
                }));return;
            }
            if(label=="add") {
                id begin,end,delta;words>>begin>>end>>delta;if(!(words && write)) {bad_input();return;}
                auto call=write->try_add_range(begin,end,delta);if(!call) {bad_input();return;}
                start_call(host,std::move(*call),std::function<void(wire::reply)>(done));return;
            }
            if(label=="stale" || label=="raw_write" || label=="forged_mode" || label=="bad_method" || label=="bad_type" || label=="wrong_object" || label=="wrong_token" || label=="bad_version") {
                if(!(label=="stale" ? have_old : have)) {bad_input();return;}auto g=label=="stale" ? old:current;
                if(label=="forged_mode") g.mode=access_mode::exclusive;
                if(label=="wrong_object") ++g.key.incarnation;
                if(label=="wrong_token") g.secret[0]^=1;
                auto method=(label=="raw_write" || label=="forged_mode" || label=="bad_type") ? capstan::method::add_range:capstan::method::version;
                if(label=="bad_method") method=static_cast<capstan::method>(999);
                std::vector<wire::value> a;if(method==capstan::method::add_range) a={wire::u(0),wire::u(1),wire::u(1)};
                if(label=="bad_type") a[0]=wire::value::boolean(true);
                auto packet=wire::encode_call({g,raw_request++,method},a);
                if(label=="bad_version") packet[4]=2;
                start_io(host,std::move(packet),[this](io_result io){finish(std::move(io));});return;
            }
            if(label=="stats" || label=="table_stats") {
                start_io(label=="stats" ? host:table,wire::command(wire::stats),[this](io_result io){finish(std::move(io));});return;
            }
            if(label=="wire") {
                auto h=host_socket.stats(),t=table_socket.stats(),c=control_socket.stats();
                h.sent_bytes+=c.sent_bytes;h.received_bytes+=c.received_bytes;h.sent_frames+=c.sent_frames;
                finish({outcome::completed,status::ok,{wire::u(h.sent_bytes+h.received_bytes),wire::u(t.sent_bytes+t.received_bytes),wire::u(h.sent_frames),wire::u(t.sent_frames)}});return;
            }
            bad_input();
        } CAPSTAN_CATCH_ALL {finish({outcome::not_submitted,status::invalid,{}});}
    }
    void on_stdin() {
        char buffer[4096];auto n=::read(STDIN_FILENO,buffer,sizeof(buffer));
        if(n==0) eof=true;
        if(n>0) {
            input.append(buffer,static_cast<std::size_t>(n));
            if(input.size()>65536) {eof=true;input.clear();}
            std::size_t end;
            while((end=input.find('\n'))!=std::string::npos) {
                if(commands.size()<64) commands.push_back(input.substr(0,end));else {eof=true;input.clear();break;}
                input.erase(0,end+1);
            }
        }
        pump();
    }
};
int main(int argc,char** argv) {
    CAPSTAN_TRY {
        auto args=app::options(argc,argv);auto* loop=g_main_loop_new(nullptr,FALSE);
        client_runtime runtime(loop,app::port_option(args,"--host-port",0),app::port_option(args,"--table-port",0),app::load_credential(app::required(args,"--credential")));
        if(!runtime.host_socket.valid() || !runtime.table_socket.valid() || !runtime.control_socket.valid()) {std::cerr<<"client connection failed\n";return 1;}
        auto flags=fcntl(STDIN_FILENO,F_GETFL,0);fcntl(STDIN_FILENO,F_SETFL,flags|O_NONBLOCK);
        auto source=g_unix_fd_add(STDIN_FILENO,static_cast<GIOCondition>(G_IO_IN|G_IO_HUP),[](gint,GIOCondition,gpointer p)->gboolean {
            auto* c=static_cast<client_runtime*>(p);CAPSTAN_TRY {c->on_stdin();} CAPSTAN_CATCH_ALL {g_main_loop_quit(c->loop);return G_SOURCE_REMOVE;}
            return c->eof ? G_SOURCE_REMOVE:G_SOURCE_CONTINUE;
        },&runtime);
        std::cout<<"READY client trusted_lab=1\n"<<std::flush;
        g_main_loop_run(loop);if(!runtime.eof) g_source_remove(source);g_main_loop_unref(loop);
    } CAPSTAN_CATCH_STD(e) {std::cerr<<e.what()<<'\n';return 1;}
}
