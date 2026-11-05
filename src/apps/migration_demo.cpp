#include "common.hpp"
#include "migration_metrics.hpp"
#include <capstan/bridge/dataset.hpp>
#include <capstan/access/clipboard.hpp>
#include <capstan/migration/coordinator.hpp>
#include <capstan/protocol/random.hpp>
#include <chrono>
#include <thread>
using namespace capstan;
namespace {
bool blocking_pump=false;
std::uint64_t pump_turns=0,empty_pump_turns=0;
const char* metric_name(id command) noexcept {
    switch(command) {
    case migration::reserve_export_command:return "rpc_reserve_source";
    case migration::reserve_command:return "rpc_reserve_target";
    case migration::freeze_command:return "rpc_freeze";
    case migration::export_command:return "rpc_export";
    case migration::pull_command:return "rpc_pull";
    case migration::fence_command:return "rpc_fence";
    case migration::activate_command:return "rpc_activate";
    case migration::retire_command:return "rpc_retire";
    case migration::export_release_command:return "rpc_release";
    default:return "rpc_other";
    }
}
struct session final : io_endpoint {
    gio_client socket;app::authenticated_endpoint endpoint;bool measure=false;
    session(GMainContext* context,std::string address,unsigned short port,token credential):socket(context,address,port),endpoint(socket,credential) {
        if(!socket.valid()) app::startup_failure("could not connect migration peer");
    }
    bool submit(bytes packet,io_completion completion) noexcept override {
        if(!measure) return endpoint.submit(std::move(packet),std::move(completion));
        CAPSTAN_TRY {
            wire::codec::request request;id command=0;
            if(wire::try_decode(packet,request)) {
                if(request.method==migration::route_command && request.args.size()==4)
                    wire::try_decode(wire::binary(wire::string(request.args[3])),request);
                command=request.method;
            }
            const auto started=app::metric_stamp::now();
            return endpoint.submit(std::move(packet),[command,started,done=std::move(completion)](io_result io) mutable {
                app::emit_metric(metric_name(command),started,app::metric_stamp::now());done(std::move(io));
            });
        } CAPSTAN_CATCH_ALL {return false;}
    }
};
void require(bool ok,const char* message) {if(!ok) app::startup_failure(message);}
template<class Predicate> void pump(GMainContext* context,Predicate ready) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    auto cleanup=[](GSource* source) {g_source_destroy(source);g_source_unref(source);};
    std::unique_ptr<GSource,decltype(cleanup)> timeout(nullptr,cleanup);
    if(blocking_pump) {
        timeout.reset(g_timeout_source_new(60000));
        g_source_set_callback(timeout.get(),[](gpointer)->gboolean {return G_SOURCE_CONTINUE;},nullptr,nullptr);
        g_source_attach(timeout.get(),context);
    }
    while(!ready()) {
        ++pump_turns;
        if(blocking_pump) g_main_context_iteration(context,TRUE);
        else {
            bool dispatched=false;while(g_main_context_iteration(context,FALSE)) dispatched=true;
            if(!dispatched) ++empty_pump_turns;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        require(std::chrono::steady_clock::now()<deadline,"migration observation timed out; do not assume rollback");
    }
}
wire::reply call(GMainContext* context,io_endpoint& endpoint,bytes packet) {
    bool ready=false;wire::reply reply;
    start_io(endpoint,std::move(packet),[&](io_result io) {
        require(io.state==outcome::completed && wire::try_decode_reply(io.payload,reply),"unknown control result");ready=true;
    });
    pump(context,[&]{return ready;});return reply;
}
}
int main(int argc,char** argv) {
    CAPSTAN_TRY {
        auto args=app::options(argc,argv);auto config=app::load_config(app::required(args,"--config"));
        const bool profile=app::option_number(args,"--profile",0)!=0;
        const auto pump_mode=args.count("--pump-mode") ? args.at("--pump-mode") : "poll";
        require(pump_mode=="poll" || pump_mode=="blocking","invalid pump mode");blocking_pump=pump_mode=="blocking";
        const auto inflight=args.count("--inflight") ? args.at("--inflight") : "none";
        require(inflight=="none" || inflight=="read" || inflight=="write","invalid inflight mode");
        const auto inflight_count=inflight=="none" ? 0 : app::option_number(args,"--inflight-count",1);
        require(inflight=="none" || (inflight_count>0 && inflight_count<=8),"inflight-count must be 1..8");
        auto source_port=app::port_option(args,"--source-port",0),target_port=app::port_option(args,"--target-port",0);
        auto source_address=args.count("--source-address") ? args.at("--source-address") : "127.0.0.1";
        auto target_address=args.count("--target-address") ? args.at("--target-address") : "127.0.0.1";
        auto* context=g_main_context_default();
        auto source=std::make_shared<session>(context,source_address,source_port,config.credentials[2]);
        auto target=std::make_shared<session>(context,target_address,target_port,config.credentials[2]);
        auto a=call(context,*source,wire::command(migration::host_info_command));
        auto b=call(context,*target,wire::command(migration::host_info_command));
        require(a.code==status::ok && b.code==status::ok && a.values.size()==4 && b.values.size()==4,"invalid Host identity");
        grant initial;initial.key=config.key;initial.epoch=1;initial.holder=1;initial.mode=access_mode::exclusive;
        require(try_random_id(initial.grant_id) && try_random_token(initial.secret),"entropy unavailable");
        transition install{config.key,config.control,1,0,1,false,{initial}};
        require(call(context,*source,wire::command(wire::set_authority,{wire::value::string(wire::binary(wire::encode_transition(install)))})).code==status::ok,"source must be a fresh demo Host");
        auto source_business=std::make_shared<session>(context,source_address,source_port,config.credentials[0]);
        auto target_business=std::make_shared<session>(context,target_address,target_port,config.credentials[0]);
        auto& before=*source_business;
        require(call(context,before,wire::encode_call({initial,10001,method::add_range},{wire::u(0),wire::u(1),wire::u(17)})).code==status::ok,"initial write failed");
        auto expected=call(context,before,wire::encode_call({initial,10002,method::summary}));require(expected.code==status::ok,"initial summary failed");
        exclusive_capability local(initial);
        migration::snapshot_ref snapshot;bool copied=false;migration::copy_ref copy_reference;
        const auto copy_start=std::chrono::steady_clock::now();
        dynabridge::ff::start_invocation(capstan::copy(source_business,local,copy_reference),[&](flux_foundry::result_t<migration::snapshot_ref,dynabridge::async_error> r) noexcept {
            require(r.has_value(),"Copy failed");snapshot=std::move(r.value());copied=true;
            require(bool(local.try_add_range(0,1,1)),"Copy borrow was not released before continuation");
        });
        pump(context,[&]{return copied;});
        const auto copy_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-copy_start).count();
        double paste_seconds[2]{};
        bool queried=false;dynabridge::ff::start_invocation(capstan::query_copy(source_business,copy_reference),[&](flux_foundry::result_t<migration::snapshot_ref,dynabridge::async_error> r) noexcept {
            require(r.has_value() && r.value()==snapshot,"Copy query changed its saved value");queried=true;
        });pump(context,[&]{return queried;});
        require(call(context,before,wire::encode_call({initial,10003,method::add_range},{wire::u(0),wire::u(1),wire::u(23)})).code==status::ok,"post-Copy write failed");
        for(id sequence=1;sequence<=2;++sequence) {
            object_key clone_key{config.key.cluster,config.key.incarnation,config.key.object+sequence};
            grant clone;bool pasted=false;
            const auto paste_start=std::chrono::steady_clock::now();
            dynabridge::ff::start_invocation(capstan::paste(target_business,snapshot,clone_key,sequence,source_address,source_port),[&](flux_foundry::result_t<grant,dynabridge::async_error> r) noexcept {
                require(r.has_value(),"Paste Copy failed");clone=std::move(r.value());pasted=true;
            });
            pump(context,[&]{return pasted;});
            paste_seconds[sequence-1]=std::chrono::duration<double>(std::chrono::steady_clock::now()-paste_start).count();
            bool checked=false;dynabridge::ff::start_invocation(capstan::query_clone(target_business,sequence),[&](flux_foundry::result_t<clone_receipt,dynabridge::async_error> r) noexcept {
                require(r.has_value() && r.value().state==clone_receipt::phase::installed && r.value().original==clone,"clone query lost its creation receipt");checked=true;
            });pump(context,[&]{return checked;});
            auto actual=call(context,*target_business,migration::route(clone_key,wire::encode_call({clone,10001,method::summary})));
            require(actual.code==status::ok && wire::encode_reply(actual.state,actual.code,actual.values)==wire::encode_reply(expected.state,expected.code,expected.values),"Paste did not preserve Copy-time data");
        }
        bool aborted=false;dynabridge::ff::start_invocation(capstan::abort_clone(target_business,99),[&](flux_foundry::result_t<bool,dynabridge::async_error> r) noexcept {
            require(r.has_value() && r.value(),"clone Abort tombstone failed");aborted=true;
        });pump(context,[&]{return aborted;});
        std::cout<<"COPY + PASTE twice: two independent object keys; both retain Copy-time data after source write\n";
        bool released=false;dynabridge::ff::start_invocation(capstan::release_snapshot(source_business,snapshot),[&](flux_foundry::result_t<bool,dynabridge::async_error> r) noexcept {
            require(r.has_value() && r.value(),"snapshot release failed");released=true;
        });pump(context,[&]{return released;});
        expected=call(context,before,wire::encode_call({initial,10004,method::summary}));require(expected.code==status::ok,"pre-Cut summary failed");
        std::vector<std::shared_ptr<session>> held_sessions;
        std::size_t held_completed=0;
        const auto held_method=inflight=="write" ? method::add_range : method::summary;
        const auto held_args=inflight=="write" ? std::vector<wire::value>{wire::u(0),wire::u(1),wire::u(0)} : std::vector<wire::value>{};
        for(id i=0;i<inflight_count;++i) {
            auto held=std::make_shared<session>(context,source_address,source_port,config.credentials[0]);
            start_io(*held,wire::encode_call({initial,20000+i,held_method},held_args),[&](io_result io) {
                wire::reply reply;require(io.state==outcome::completed && wire::try_decode_reply(io.payload,reply) && reply.code==status::ok,"inflight operation failed");++held_completed;
            },held);
            held_sessions.push_back(std::move(held));
        }
        if(inflight_count) {
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            for(;;) {
                auto stats=call(context,*source,wire::command(wire::stats));
                require(stats.code==status::ok && stats.values.size()==7,"invalid occupancy observation");
                if(wire::number(stats.values[4])==inflight_count) break;
                require(!held_completed && std::chrono::steady_clock::now()<deadline,"inflight work ended before measurement; increase the hold delay");
            }
        }
        auto successor=initial;++successor.epoch;
        require(try_random_id(successor.grant_id) && try_random_token(successor.secret),"entropy unavailable");
        migration::ticket ticket{config.control,1,wire::number(a.values[0]),wire::number(a.values[1]),wire::number(b.values[0]),wire::number(b.values[1]),wire::number(a.values[2]),initial};
        auto move=std::make_shared<migration::move_operation>(ticket,successor,source,target,source_address,source_port,wire::number(a.values[3]));
        std::cout<<"CUT prepared: no source Close until Paste submission\n";
        bool completed=false;wire::reply receipt;
        source->measure=target->measure=profile;
        const auto turns_before=pump_turns,empty_before=empty_pump_turns;
        const auto cpu_start=app::metric_stamp::now(profile);
        auto start=std::chrono::steady_clock::now();
        auto observer=[&](io_result io) {require(io.state==outcome::completed && wire::try_decode_reply(io.payload,receipt),"unknown migration observation");completed=true;};
        status prepared;auto cut=cut_plan::prepare(local,move,prepared);
        require(cut && prepared==status::ok && !cut->sealed(),"Cut preparation failed");
        {auto abandoned=cut->paste();} // An invocation destroyed before submit cannot seal.
        require(!cut->sealed() && bool(local.try_summary()),"pre-submit Cut changed authority");
        dynabridge::ff::start_invocation(cut->paste(),[&](flux_foundry::result_t<io_result,dynabridge::async_error> r) noexcept {
            require(r.has_value(),"Cut Paste FF submission failed");observer(std::move(r.value()));
        });
        pump(context,[&]{return completed;});require(cut->sealed() && !local.try_summary(),"old local handle reopened");
        require(receipt.state==outcome::completed && receipt.code==status::ok && move->ready(),"migration requires same-session resolution");
        auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        app::emit_metric("driver_cut",cpu_start,app::metric_stamp::now(profile));
        if(profile) std::cout<<"PUMP turns="<<pump_turns-turns_before<<" empty_turns="<<empty_pump_turns-empty_before<<'\n';
        source->measure=target->measure=false;
        pump(context,[&]{return held_completed==inflight_count;});
        session after(context,target_address,target_port,config.credentials[0]);
        auto actual=call(context,after,wire::encode_call({successor,3,method::summary}));
        dataset_backend::context conversion;
        require(expected.values.size()==1 && actual.code==status::ok && actual.values.size()==1,"invalid summary after migration");
        auto expected_summary=dynabridge::from_optional<summary_t>(conversion,expected.values[0]);
        auto actual_summary=dynabridge::from_optional<summary_t>(conversion,actual.values[0]);
        require(bool(expected_summary) && bool(actual_summary),"summary decode failed");
        if(inflight=="write") expected_summary->version+=inflight_count;
        require(*expected_summary==*actual_summary,"snapshot changed during migration");
        for(id i=0;i<inflight_count;++i)
            require(call(context,after,wire::encode_call({successor,20000+i,held_method},held_args)).code==status::duplicate,"inflight request history was lost");
        require(call(context,after,wire::encode_call({successor,10001,method::add_range},{wire::u(0),wire::u(1),wire::u(17)})).code==status::duplicate,"request history was lost");
        require(call(context,before,wire::encode_call({initial,4,method::version})).code==status::closed,"source reopened after migration");
        auto source_info=call(context,*source,wire::command(wire::info));
        require(source_info.values.size()==4 && wire::number(source_info.values[3])==0,"old native storage was not retired");
        std::cout<<"PASTE ready: same object key; generation="<<ticket.generation+1<<" epoch="<<successor.epoch<<"; summary matches; dedup preserved; source retired\n";
        std::cout<<"MEASURE package_bytes="<<move->transferred().size<<" seconds="<<elapsed<<" MiB_per_second="<<(move->transferred().size/1048576.0/elapsed)<<" close_to_activation_seconds="<<move->observed_close_to_activation_seconds()<<"\n";
        std::cout<<"PHASE copy_seconds="<<copy_seconds<<" first_paste_seconds="<<paste_seconds[0]<<" second_paste_seconds="<<paste_seconds[1]<<"\n";
        std::cout<<"PASS host_to_host_migration\n";
    } CAPSTAN_CATCH_STD(error) {std::cerr<<error.what()<<'\n';return 1;}
}
