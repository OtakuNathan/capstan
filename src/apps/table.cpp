#include <capstan/error_policy.hpp>
#include "common.hpp"
#include <capstan/authority/directory.hpp>
#include <capstan/protocol/random.hpp>
using namespace capstan;
struct table_runtime {
    app::config config;
    GMainContext* context;
    directory_entry directory;
    gio_client host_socket;
    app::authenticated_endpoint host;
    std::vector<grant> published;
    id control_calls=0,fetches=0;
    struct command_record {bytes payload; bytes response;};
    std::map<std::pair<id,id>,command_record> history;
    bool changing=false;
    table_runtime(app::config c,GMainContext* ctx,unsigned short port)
        :config(c),context(ctx),directory(c.key,c.control),host_socket(ctx,"127.0.0.1",port),host(host_socket,c.credentials[2]){}
    struct transition_operation {
        transition target;
        io_completion done;
        std::pair<id,id> command;
    };
    void advance(std::shared_ptr<transition_operation> op) {
        ++control_calls;
        auto b=wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition(op->target)))});
        start_io(host,std::move(b),[this,op](io_result io) {
            if(io.state!=outcome::completed) {
                // Keep Directory pending and grants unpublished. Loss of Host
                // connection does not establish rollback or authorize a writer.
                auto response=wire::encode_reply(outcome::unknown,status::pending);
                history.at(op->command).response=response;
                op->done({outcome::completed,std::move(response)}); return;
            }
            CAPSTAN_TRY {
                auto unknown=[&] {
                    auto response=wire::encode_reply(outcome::unknown,status::pending);
                    history.at(op->command).response=response;op->done({outcome::completed,std::move(response)});
                };
                wire::reply r;if(!wire::try_decode_reply(io.payload,r)) {unknown();return;}
                // Host retains this request until Install. An unexpected
                // Pending/error is conservative OutcomeUnknown, never a grant.
                acknowledgement ack;
                if(r.code!=status::ok || r.values.size()!=1 || !wire::is_string(r.values[0]) ||
                   !wire::try_decode_transition(wire::binary(wire::string(r.values[0])),ack.installed) ||
                   directory.receive(ack)!=status::ok || directory.take_grants(published)!=status::ok) {unknown();return;}
                changing=false;
                auto response=wire::encode_reply(outcome::completed,status::ok,{wire::u(directory.epoch()),wire::u(published.size())});
                history.at(op->command).response=response;
                std::cout<<"GRANT tx="<<op->target.tx<<" epoch="<<directory.epoch()<<" holders="<<published.size()<<'\n'<<std::flush;
                op->done({outcome::completed,std::move(response)});
            } CAPSTAN_CATCH_ALL {
                auto response=wire::encode_reply(outcome::unknown,status::pending);
                history.at(op->command).response=response;
                op->done({outcome::completed,std::move(response)});
            }
        });
    }
    void handle(id,bytes packet,io_completion done) {
        CAPSTAN_TRY {
            app::authenticated auth;wire::codec::request r;
            auto invalid=[&]{app::reply(std::move(done),status::invalid);};
            if(!app::try_authenticate(config,packet,auth) || !wire::try_decode(auth.payload,r)) {invalid();return;}
            if(r.method==wire::info) {app::reply(std::move(done),status::ok,{wire::u(config.key.cluster),wire::u(config.key.incarnation),wire::u(config.key.object)});return;}
            if(r.method==wire::stats) {app::reply(std::move(done),status::ok,{wire::u(control_calls),wire::u(fetches),wire::u(0),wire::u(directory.epoch())});return;}
            if(r.method==wire::fetch_grant) {
                ++fetches;
                if(changing) {app::reply(std::move(done),status::busy);return;}
                for(const auto& g:published) if(g.holder==auth.holder) {app::reply(std::move(done),status::ok,{wire::value::string(wire::encode_grant(g))});return;}
                app::reply(std::move(done),status::bad_grant);return;
            }
            const bool release=r.method==2002;
            if(!release && r.method!=wire::configure_grants) {invalid();return;}
            if(!release && auth.holder!=1) {app::reply(std::move(done),status::forbidden);return;}
            if(r.args.size()<2 || !wire::is_number(r.args[0])) {invalid();return;}
            auto command=std::make_pair(auth.holder,wire::number(r.args[0])); if(!command.second) {invalid();return;}
            auto prior=history.find(command);
            if(prior!=history.end()) {
                if(prior->second.payload!=auth.payload) app::reply(std::move(done),status::conflict);
                else if(prior->second.response.empty()) app::reply(std::move(done),status::pending);
                else done({outcome::completed,prior->second.response});
                return;
            }
            if(changing) {app::reply(std::move(done),status::busy);return;}
            if(history.size()>=4096) {app::reply(std::move(done),status::exhausted);return;}
            bool destroy=false; std::vector<grant> target;
            if(release) {
                grant closed;if(r.args.size()!=2 || !wire::is_string(r.args[1]) || !wire::try_decode_grant(wire::string(r.args[1]),closed)) {invalid();return;}
                auto it=std::find(published.begin(),published.end(),closed);
                if(it==published.end() || closed.holder!=auth.holder) {app::reply(std::move(done),status::bad_grant);return;}
                for(const auto& g:published) if(g.holder!=auth.holder) target.push_back(g);
            } else {
                if(!wire::is_boolean(r.args[1]) || r.args.size()%2!=0 || r.args.size()>6) {invalid();return;} destroy=wire::boolean(r.args[1]);
                for(std::size_t i=2;i<r.args.size();i+=2) {
                    if(!wire::is_number(r.args[i]) || !wire::is_number(r.args[i+1])) {invalid();return;}
                    grant g; g.holder=wire::number(r.args[i]);auto m=wire::number(r.args[i+1]);
                    if(g.holder<1 || g.holder>2 || (m!=1 && m!=2)) {invalid();return;} g.mode=static_cast<access_mode>(m); target.push_back(g);
                }
            }
            for(auto& g:target) {if(!try_random_id(g.grant_id) || !try_random_token(g.secret)) {app::reply(std::move(done),status::exhausted);return;}}
            auto op=std::make_shared<transition_operation>();op->command=command;
            history.emplace(command,command_record{auth.payload,{}});
            status code;
            CAPSTAN_TRY {code=directory.begin(std::move(target),destroy,op->target);}
            CAPSTAN_CATCH_ALL {history.erase(command);CAPSTAN_RETHROW;}
            if(code!=status::ok) {history.erase(command);app::reply(std::move(done),code);return;}
            changing=true;op->done=std::move(done);advance(op);
        } CAPSTAN_CATCH_ALL {if(done) app::reply(std::move(done),status::invalid);}
    }
};
int main(int argc,char** argv) {
    CAPSTAN_TRY {
        auto args=app::options(argc,argv);auto config=app::load_config(app::required(args,"--config"));auto* context=g_main_context_default();
        table_runtime runtime(config,context,app::port_option(args,"--host-port",0));
        if(!runtime.host_socket.valid()) {std::cerr<<runtime.host_socket.setup_error()<<'\n';return 1;}
        gio_server server(context,app::port_option(args,"--port",0),[&](id c,bytes b,io_completion done){runtime.handle(c,std::move(b),std::move(done));});
        if(!server.valid()) {std::cerr<<server.setup_error()<<'\n';return 1;}
        std::cout<<"READY table port="<<server.port()<<" trusted_lab=1\n"<<std::flush;
        auto* loop=g_main_loop_new(context,FALSE);g_main_loop_run(loop);g_main_loop_unref(loop);
    } CAPSTAN_CATCH_STD(e) {std::cerr<<e.what()<<'\n';return 1;}
}
