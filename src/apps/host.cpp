#include <capstan/error_policy.hpp>
#include "common.hpp"
#include "worker.hpp"
#include "authority_replies.hpp"
#include <capstan/authority/host.hpp>
#include <capstan/protocol/random.hpp>
#include <capstan/bridge/dataset.hpp>
#include <chrono>
#include <thread>
using namespace capstan;
struct host_runtime {
    app::config config;
    GMainContext* context;
    host_authority authority;
    app::authority_replies control_replies{authority};
    std::unique_ptr<dataset_binding> dataset;
    app::business_worker worker;
    struct pending { wire::call call; io_completion done; };
    std::map<id,std::shared_ptr<pending>> operations;
    unsigned delay_ms;
    id admitted=0,completed=0,rejected=0,control_calls=0,destroyed=0;
    host_runtime(app::config c,GMainContext* ctx,std::size_t size,unsigned delay
#ifdef CAPSTAN_BENCHMARK_BUILD
        ,host_authority::benchmark_validation validation
#endif
        )
        :config(c),context(ctx),authority(c.key,c.control
#ifdef CAPSTAN_BENCHMARK_BUILD
        ,validation
#endif
        ),dataset(new dataset_binding(c.key,size,7)),delay_ms(delay) {}
    ~host_runtime() { worker.close(); }
    void collect_resource() {
        if(authority.inspect().gate==host_authority::phase::dead && dataset) {
            dataset.reset(); ++destroyed; std::cout<<"DESTROY active=0\n"<<std::flush;
        }
        control_replies.flush();
    }
    void kick() {
        host_authority::operation op;
        if(operations.empty() || authority.inspect().executing) return;
        auto pin=operations.begin()->first;
        auto frame=operations.begin()->second;
        // Construct the dispatch task before starting the operation. If task
        // allocation fails, pin remains queued; no unpinned native execution.
        CAPSTAN_TRY {
        auto task=flux_foundry::task_wrapper_sbo([this,pin,frame]() noexcept {
            bytes response;
            CAPSTAN_TRY {
                if(delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                wire::value result;
                bool ok=dataset->try_dispatch(frame->call.header.operation,frame->call.header.capability,frame->call.args,result);
                response=wire::encode_reply(outcome::completed,ok ? status::ok : status::invalid,ok ? std::vector<wire::value>{result} : std::vector<wire::value>{});
            } CAPSTAN_CATCH_ALL { response=wire::encode_reply(outcome::completed,status::invalid); }
            app::post(context,[this,pin,response=std::move(response)]() mutable {
                auto it=operations.find(pin); auto done=std::move(it->second->done); operations.erase(it);
                if(authority.complete(pin)!=status::ok) std::terminate();
                ++completed;
                std::cout<<"COMPLETE pin="<<pin<<" active="<<authority.inspect().active<<'\n'<<std::flush;
                collect_resource(); kick(); done({outcome::completed,std::move(response)});
            });
        });
        if(!authority.start_next(op)) return;
        if(!worker.try_dispatch(std::move(task))) {
            // start_next already marked this pin executing. A false submission
            // guarantees the task never ran and cannot later run; end the pin
            // here, just as an operation terminating before business access.
            if(authority.complete(pin)!=status::ok) std::terminate();
            auto done=std::move(frame->done);operations.erase(pin);++rejected;
            collect_resource();app::reply(std::move(done),status::rejected);kick();
        }
        } CAPSTAN_CATCH_ALL {
            authority.dispatch_failed(pin);
            auto done=std::move(frame->done);operations.erase(pin);++rejected;
            collect_resource();app::reply(std::move(done),status::rejected);kick();
        }
    }
    void handle(id,bytes frame,io_completion done) {
        CAPSTAN_TRY {
            app::authenticated auth;wire::codec::request decoded;
            if(!app::try_authenticate(config,frame,auth) || !wire::try_decode(auth.payload,decoded)) {app::reply(std::move(done),status::invalid);return;}
            if(decoded.method==wire::info) {
                app::reply(std::move(done),status::ok,{wire::u(config.key.cluster),wire::u(config.key.incarnation),wire::u(config.key.object),wire::u(dataset ? dataset->resource_bytes():0)}); return;
            }
            if(decoded.method==wire::query_current_authority || decoded.method==wire::fetch_my_grant) {
                ++control_calls;
                const bool administrative=decoded.method==wire::query_current_authority;
                const auto offset=administrative ? 1u : 0u;
                if(decoded.args.size()!=offset+3) {app::reply(std::move(done),status::invalid);return;}
                for(const auto& a:decoded.args) if(!wire::is_number(a)) {app::reply(std::move(done),status::invalid);return;}
                if(administrative && (auth.holder!=3 || wire::number(decoded.args[0])!=config.control)) {app::reply(std::move(done),status::wrong_control);return;}
                object_key key{wire::number(decoded.args[offset]),wire::number(decoded.args[offset+1]),wire::number(decoded.args[offset+2])};
                if(key!=config.key) {app::reply(std::move(done),status::wrong_object);return;}
                if(administrative) {
                    auto snapshot=authority.inspect();
                    authority_view view{config.key,snapshot.epoch,static_cast<unsigned>(snapshot.gate),std::move(snapshot.grants)};
                    app::reply(std::move(done),status::ok,{wire::value::string(wire::encode_authority_view(view))});
                } else {
                    grant current;const auto code=authority.fetch_current(key,auth.holder,current);
                    app::reply(std::move(done),code,code==status::ok ? std::vector<wire::value>{wire::value::string(wire::encode_grant(current))} : std::vector<wire::value>{});
                }
                return;
            }
            if(decoded.method==wire::transfer_exclusive || decoded.method==wire::query_transfer || decoded.method==wire::claim_transfer) {
                ++control_calls;
                if(decoded.args.size()!=1 || !wire::is_string(decoded.args[0])) {app::reply(std::move(done),status::invalid);return;}
                if(decoded.method==wire::transfer_exclusive) {
                    transfer_request request;
                    if(!wire::try_decode_transfer_request(wire::string(decoded.args[0]),request)) {app::reply(std::move(done),status::invalid);return;}
                    const auto code=control_replies.transfer(request,auth.holder,[](grant& successor) {
                        return try_random_id(successor.grant_id) && try_random_token(successor.secret);
                    },[this,transaction=request.transaction,done=std::move(done)](io_result io) mutable {
                        auto timing=authority.timing(transaction);
                        if(timing.install_ns) std::cout<<"TRANSFER_TIMING issuer="<<transaction.issuer<<" tx="<<transaction.sequence
                            <<" close_ns="<<timing.close_ns<<" drain_ns="<<timing.drain_ns<<" install_ns="<<timing.install_ns
                            <<" reply_ns="<<host_authority::monotonic_ns()<<'\n'<<std::flush;
                        done(std::move(io));
                    });
                    std::cout<<"TRANSFER issuer="<<request.transaction.issuer<<" tx="<<request.transaction.sequence
                             <<" from="<<request.source.holder<<" to="<<request.recipient<<" status="<<status_name(code)
                             <<" epoch="<<authority.inspect().epoch<<" active="<<authority.inspect().active<<'\n'<<std::flush;
                    collect_resource();return;
                }
                transfer_ref reference;
                if(!wire::try_decode_transfer_ref(wire::string(decoded.args[0]),reference)) {app::reply(std::move(done),status::invalid);return;}
                if(decoded.method==wire::query_transfer) {
                    auto result=authority.query_transfer(reference,auth.holder,auth.holder==3);
                    app::reply(std::move(done),result.code,{wire::value::string(wire::encode_transfer_receipt(result))});
                } else {
                    grant successor;auto code=authority.claim_transfer(reference,auth.holder,successor);
                    app::reply(std::move(done),code,code==status::ok ? std::vector<wire::value>{wire::value::string(wire::encode_grant(successor))} : std::vector<wire::value>{});
                }
                return;
            }
            if(decoded.method==wire::set_authority || decoded.method==wire::query_ack) {
                if(auth.holder!=3) {app::reply(std::move(done),status::wrong_control);return;}
                ++control_calls;
                if(decoded.args.size()!=1) {app::reply(std::move(done),status::invalid);return;}
                id tx=0; status code;
                if(decoded.method==wire::set_authority) {
                    transition t;
                    if(!wire::is_string(decoded.args[0]) || !wire::try_decode_transition(wire::binary(wire::string(decoded.args[0])),t)) {app::reply(std::move(done),status::invalid);return;}
                    code=control_replies.set(t,std::move(done));
                    std::cout<<"TRANSITION tx="<<t.tx<<" epoch="<<authority.inspect().epoch<<" status="<<status_name(code)<<" active="<<authority.inspect().active<<'\n'<<std::flush;
                    collect_resource(); return;
                } else {if(!wire::is_number(decoded.args[0])) {app::reply(std::move(done),status::invalid);return;} tx=wire::number(decoded.args[0]);code=status::ok;}
                collect_resource();
                acknowledgement ack;
                if(code==status::ok) code=authority.ack(tx,ack);
                app::reply(std::move(done),code,code==status::ok ? std::vector<wire::value>{wire::value::string(wire::binary(wire::encode_transition(ack.installed)))} : std::vector<wire::value>{}); return;
            }
            if(decoded.method==wire::stats) {
                auto s=authority.inspect();
                app::reply(std::move(done),status::ok,{wire::u(admitted),wire::u(completed),wire::u(rejected),wire::u(control_calls),wire::u(s.active),wire::u(s.epoch),wire::u(destroyed)}); return;
            }
            wire::call call;if(!wire::try_decode_call(auth.payload,call)) {app::reply(std::move(done),status::invalid);return;}
            // Store payload before admission so allocation failure cannot leave
            // an accepted operation without its corresponding execution frame.
            auto frame_owner=std::make_shared<pending>();
            frame_owner->call=std::move(call);frame_owner->done=std::move(done);
            host_authority::admission admission{status::invalid};
            CAPSTAN_TRY {admission=authority.admit(frame_owner->call.header,auth.holder);}
            CAPSTAN_CATCH_ALL {app::reply(std::move(frame_owner->done),status::rejected);return;}
            if(admission.code!=status::ok) {
                ++rejected; std::cout<<"REJECT status="<<status_name(admission.code)<<'\n'<<std::flush;
                app::reply(std::move(frame_owner->done),admission.code); return;
            }
            CAPSTAN_TRY { operations.emplace(admission.pin,frame_owner); }
            CAPSTAN_CATCH_ALL {authority.dispatch_failed(admission.pin); collect_resource(); app::reply(std::move(frame_owner->done),status::rejected); return;}
            ++admitted; std::cout<<"ADMIT pin="<<admission.pin<<" epoch="<<authority.inspect().epoch<<" active="<<authority.inspect().active<<'\n'<<std::flush;
            kick();
        } CAPSTAN_CATCH_ALL { if(done) app::reply(std::move(done),status::invalid); }
    }
};
int main(int argc,char** argv) {
    CAPSTAN_TRY {
        auto args=app::options(argc,argv); auto config=app::load_config(app::required(args,"--config"));
        auto size=app::option_number(args,"--bytes",64*1024*1024); if(!size || size>256*1024*1024) app::startup_failure("dataset size out of bounds");
        auto delay=app::option_number(args,"--work-delay-ms",0); if(delay>1000) app::startup_failure("delay out of bounds");
#ifdef CAPSTAN_BENCHMARK_BUILD
        if(app::required(args,"--validation")!="checked" && app::required(args,"--validation")!="omit") app::startup_failure("validation must be checked or omit");
#endif
        auto* context=g_main_context_default(); host_runtime runtime(config,context,size,static_cast<unsigned>(delay)
#ifdef CAPSTAN_BENCHMARK_BUILD
        ,app::required(args,"--validation")=="checked" ? host_authority::benchmark_validation::checked : host_authority::benchmark_validation::omit_grant_checks
#endif
        );
        if(!runtime.worker.valid()) {std::cerr<<"business worker startup failed\n";return 1;}
        gio_server server(context,app::port_option(args,"--port",0),[&](id c,bytes b,io_completion done){runtime.handle(c,std::move(b),std::move(done));});
        if(!server.valid()) {std::cerr<<server.setup_error()<<'\n';return 1;}
        std::cout<<"READY host port="<<server.port()<<" resource_bytes="<<size<<" trusted_lab=1\n"<<std::flush;
        auto* loop=g_main_loop_new(context,FALSE);g_main_loop_run(loop);g_main_loop_unref(loop);
    } CAPSTAN_CATCH_STD(e) {std::cerr<<e.what()<<'\n';return 1;}
}
