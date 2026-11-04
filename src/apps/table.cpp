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
    id control_calls=0,fetches=0;
    struct command_record {bytes payload;bytes response;};
    std::map<std::pair<id,id>,command_record> history;
    bool changing=false;
    table_runtime(app::config c,GMainContext* ctx,unsigned short port)
        :config(c),context(ctx),directory(c.key,c.control),host_socket(ctx,"127.0.0.1",port),host(host_socket,c.credentials[2]){}
    struct transition_operation {
        transition target;
        io_completion done;
        std::pair<id,id> command;
        bool started=false,ever_uncertain=false;
    };
    std::shared_ptr<transition_operation> uncertain_operation;
    using snapshot_done=std::function<void(outcome,status,authority_view)>;
    void snapshot(snapshot_done done) {
        ++control_calls;
        auto packet=wire::command(wire::query_current_authority,{wire::u(config.control),wire::u(config.key.cluster),wire::u(config.key.incarnation),wire::u(config.key.object)});
        start_io(host,std::move(packet),[this,done=std::move(done)](io_result io) mutable {
            if(io.state!=outcome::completed) {done(io.state,status::busy,{});return;}
            wire::reply response;authority_view view;
            if(!wire::try_decode_reply(io.payload,response)) {done(outcome::unknown,status::invalid,{});return;}
            if(response.code!=status::ok) {done(response.state,response.code,{});return;}
            if(response.state!=outcome::completed || response.values.size()!=1 || !wire::is_string(response.values[0]) ||
               !wire::try_decode_authority_view(wire::string(response.values[0]),view) || view.key!=config.key) {
                done(outcome::unknown,status::invalid,{});return;
            }
            auto observed=directory.observe(view.epoch,view.gate==2);
            if(observed!=status::ok) {done(outcome::rejected,observed,{});return;}
            done(outcome::completed,status::ok,std::move(view));
        });
    }
    void finish(std::shared_ptr<transition_operation> op,outcome state,status code,std::vector<wire::value> values={},bool uncertain=false) {
        auto response=wire::encode_reply(state,code,values);
        history.at(op->command).response=response;
        changing=uncertain;
        if(uncertain) {op->ever_uncertain=true;uncertain_operation=op;}else uncertain_operation.reset();
        auto done=std::move(op->done);
        if(done) done({outcome::completed,std::move(response)});
    }
    void advance(std::shared_ptr<transition_operation> op) {
        ++control_calls;
        auto b=wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition(op->target)))});
        start_io(host,std::move(b),[this,op](io_result io) {on_ack(op,std::move(io));});
    }
    void on_ack(std::shared_ptr<transition_operation> op,io_result io) {
        auto unknown=[&]{finish(op,outcome::unknown,status::pending,{},true);};
        if(io.state==outcome::not_submitted) {
            // Failure of a retry says nothing about the earlier attempt.
            if(op->ever_uncertain) {unknown();return;}
            directory.reject(op->target,status::rejected);finish(op,outcome::not_submitted,status::rejected);return;
        }
        if(io.state!=outcome::completed) {unknown();return;}
        CAPSTAN_TRY {
            wire::reply r;acknowledgement ack;
            if(!wire::try_decode_reply(io.payload,r)) {unknown();return;}
            // A reliable, matching rejection means this submission did not
            // commit. It ends this decision; never rebase/retry its target.
            if(r.state==outcome::rejected && r.code!=status::ok && r.code!=status::pending) {
                // A full waiter adapter can reject even a duplicate of an
                // accepted transaction. Resolve prior uncertainty by ACK/query.
                if(op->ever_uncertain) {unknown();return;}
                directory.reject(op->target,r.code);finish(op,r.state,r.code);return;
            }
            if(r.state!=outcome::completed || r.code!=status::ok || r.values.size()!=1 || !wire::is_string(r.values[0]) ||
               !wire::try_decode_transition(wire::binary(wire::string(r.values[0])),ack.installed) || !(ack.installed==op->target)) {unknown();return;}
            auto received=directory.receive(ack);
            if(received!=status::ok && received!=status::stale && received!=status::duplicate) {unknown();return;}
            std::vector<grant> historical;
            if(received==status::ok || received==status::duplicate) directory.take_grants(historical); // History only, never a fetch cache.
            snapshot([this,op](outcome state,status code,authority_view current) {
                if(code!=status::ok) {finish(op,state,code);return;}
                auto installed=op->target.next_epoch;
                if(current.epoch!=installed || current.grants!=op->target.grants) {
                    finish(op,outcome::completed,status::superseded,{wire::u(installed),wire::u(current.epoch)});return;
                }
                if(current.gate==1) {finish(op,outcome::completed,status::closed,{wire::u(installed)});return;}
                std::cout<<"GRANT tx="<<op->target.tx<<" epoch="<<current.epoch<<" holders="<<current.grants.size()<<'\n'<<std::flush;
                finish(op,outcome::completed,status::ok,{wire::u(current.epoch),wire::u(current.grants.size())});
            });
        } CAPSTAN_CATCH_ALL {unknown();}
    }
    void handle(id,bytes packet,io_completion done) {
        CAPSTAN_TRY {
            app::authenticated auth;wire::codec::request r;
            auto invalid=[&]{app::reply(std::move(done),status::invalid);};
            if(!app::try_authenticate(config,packet,auth) || !wire::try_decode(auth.payload,r)) {invalid();return;}
            if(r.method==wire::info) {app::reply(std::move(done),status::ok,{wire::u(config.key.cluster),wire::u(config.key.incarnation),wire::u(config.key.object)});return;}
            if(r.method==wire::stats) {app::reply(std::move(done),status::ok,{wire::u(control_calls),wire::u(fetches),wire::u(0),wire::u(directory.epoch())});return;}
            if(r.method==2003) {
                if(auth.holder!=1) {app::reply(std::move(done),status::forbidden);return;}
                if(r.args.size()!=1 || !wire::is_boolean(r.args[0])) {invalid();return;}
                auto op=uncertain_operation;
                if(!op || op->done) {app::reply(std::move(done),status::busy);return;}
                op->done=std::move(done);
                if(wire::boolean(r.args[0])) {advance(op);return;} // Same immutable tx, never rebase.
                ++control_calls;
                start_io(host,wire::command(wire::query_ack,{wire::u(op->target.tx)}),[this,op](io_result io) {
                    wire::reply reply;
                    if(io.state!=outcome::completed || !wire::try_decode_reply(io.payload,reply) || reply.code!=status::ok) {
                        finish(op,outcome::unknown,status::pending,{},true);return;
                    }
                    on_ack(op,std::move(io));
                });return;
            }
            if(r.method==wire::fetch_grant) {
                ++fetches;
                if(!r.args.empty()) {invalid();return;}
                if(changing) {app::reply(std::move(done),status::busy);return;}
                snapshot([done=std::move(done),holder=auth.holder](outcome state,status code,authority_view view) mutable {
                    if(code!=status::ok) {done({outcome::completed,wire::encode_reply(state,code)});return;}
                    if(view.gate!=0) {app::reply(std::move(done),view.gate==2 ? status::dead : status::closed);return;}
                    for(const auto& g:view.grants) if(g.holder==holder) {app::reply(std::move(done),status::ok,{wire::value::string(wire::encode_grant(g))});return;}
                    app::reply(std::move(done),status::bad_grant);
                });return;
            }
            const bool release=r.method==2002;
            if(!release && r.method!=wire::configure_grants) {invalid();return;}
            if(!release && auth.holder!=1) {app::reply(std::move(done),status::forbidden);return;}
            if(r.args.size()<2 || !wire::is_number(r.args[0])) {invalid();return;}
            auto command=std::make_pair(auth.holder,wire::number(r.args[0]));if(!command.second) {invalid();return;}
            auto prior=history.find(command);
            if(prior!=history.end()) {
                if(prior->second.payload!=auth.payload) app::reply(std::move(done),status::conflict);
                else if(prior->second.response.empty()) app::reply(std::move(done),status::pending);
                else done({outcome::completed,prior->second.response});
                return;
            }
            if(changing) {app::reply(std::move(done),status::busy);return;}
            if(history.size()>=4096) {app::reply(std::move(done),status::exhausted);return;}
            bool destroy=false;std::vector<grant> target;grant closed;
            if(release) {
                if(r.args.size()!=2 || !wire::is_string(r.args[1]) || !wire::try_decode_grant(wire::string(r.args[1]),closed)) {invalid();return;}
                if(closed.holder!=auth.holder) {app::reply(std::move(done),status::bad_grant);return;}
            } else {
                if(!wire::is_boolean(r.args[1]) || r.args.size()%2!=0 || r.args.size()>6) {invalid();return;}
                destroy=wire::boolean(r.args[1]);
                for(std::size_t i=2;i<r.args.size();i+=2) {
                    if(!wire::is_number(r.args[i]) || !wire::is_number(r.args[i+1])) {invalid();return;}
                    grant g;g.holder=wire::number(r.args[i]);auto mode=wire::number(r.args[i+1]);
                    if(g.holder<1 || g.holder>2 || (mode!=1 && mode!=2)) {invalid();return;}
                    g.mode=static_cast<access_mode>(mode);target.push_back(g);
                }
            }
            auto op=std::make_shared<transition_operation>();op->command=command;
            history.emplace(command,command_record{auth.payload,{}});
            changing=true;op->done=std::move(done);
            snapshot([this,op,target=std::move(target),closed,release,destroy](outcome state,status code,authority_view view) mutable {
                CAPSTAN_TRY {
                    if(code!=status::ok) {finish(op,state,code);return;}
                    if(view.gate!=0) {finish(op,outcome::rejected,view.gate==2 ? status::dead : status::busy);return;}
                    if(release) {
                        if(std::find(view.grants.begin(),view.grants.end(),closed)==view.grants.end()) {finish(op,outcome::rejected,status::bad_grant);return;}
                        for(const auto& g:view.grants) if(g.holder!=closed.holder) target.push_back(g);
                    }
                    for(auto& g:target) if(!try_random_id(g.grant_id) || !try_random_token(g.secret)) {finish(op,outcome::rejected,status::exhausted);return;}
                    code=directory.begin_at(view.epoch,std::move(target),destroy,op->target);
                    if(code!=status::ok) {finish(op,outcome::rejected,code);return;}
                    op->started=true;advance(op);
                } CAPSTAN_CATCH_ALL {finish(op,outcome::unknown,status::pending,{},op->started);}
            });
        } CAPSTAN_CATCH_ALL {if(done) app::reply(std::move(done),status::invalid);}
    }
};
#ifndef CAPSTAN_TABLE_NO_MAIN
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

#endif
