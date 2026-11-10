#include "owner_host.hpp"
#include "examples/owner/single_process.hpp"
using host_runtime=basic_host_runtime<capstan::examples::single_process_owner_backend>;
#include "host_registry.hpp"
using host_registry=basic_host_registry<host_runtime>;
int main(int argc,char** argv) {
    CAPSTAN_TRY {
        auto args=app::options(argc,argv); auto config=app::load_config(app::required(args,"--config"));
        auto size=app::option_number(args,"--bytes",64*1024*1024); if(!size || size>256*1024*1024) app::startup_failure("dataset size out of bounds");
        auto delay=app::option_number(args,"--work-delay-ms",0); if(delay>1000) app::startup_failure("delay out of bounds");
        const auto history=app::option_number(args,"--history-limit",4096);
        if(!history || history>1048576) app::startup_failure("history-limit must be 1..1048576");
#ifdef CAPSTAN_BENCHMARK_BUILD
        if(app::required(args,"--validation")!="checked" && app::required(args,"--validation")!="omit") app::startup_failure("validation must be checked or omit");
#endif
        const auto empty=app::option_number(args,"--empty",0);if(empty>1) app::startup_failure("empty must be 0 or 1");
        auto* context=g_main_context_default(); host_runtime runtime(config,context,empty ? 0 : size,static_cast<unsigned>(delay),static_cast<std::size_t>(history)
#ifdef CAPSTAN_BENCHMARK_BUILD
        ,app::required(args,"--validation")=="checked" ? host_authority::benchmark_validation::checked : host_authority::benchmark_validation::omit_grant_checks
#endif
        );
        runtime.host_id=app::option_number(args,"--host-id",1);
        if(args.count("--host-instance")) runtime.host_instance=app::option_number(args,"--host-instance",0);
        else if(!try_random_id(runtime.host_instance)) app::startup_failure("Host instance entropy unavailable");
        if(!runtime.host_id || !runtime.host_instance) app::startup_failure("Host identity must be nonzero");
        runtime.export_budget=static_cast<std::size_t>(app::option_number(args,"--migration-buffer-bytes",512u*1024u*1024u));
        if(runtime.export_budget<1024 || runtime.export_budget>1024u*1024u*1024u) app::startup_failure("migration buffer budget must be 1 KiB..1 GiB");
        const auto trace=app::option_number(args,"--trace-requests",1);
        if(trace>1) app::startup_failure("trace-requests must be 0 or 1");
        runtime.trace_requests=trace!=0;
        const auto metrics=app::option_number(args,"--migration-metrics",0);
        if(metrics>1) app::startup_failure("migration-metrics must be 0 or 1");
        runtime.trace_migration_metrics=metrics!=0;
        const auto inline_read=app::option_number(args,"--inline-version",1);
        if(inline_read>1) app::startup_failure("inline-version must be 0 or 1");
        runtime.inline_version=inline_read!=0;
        if(!runtime.worker.valid()) {std::cerr<<"business worker startup failed\n";return 1;}
        host_registry registry(runtime);
        gio_server server(context,app::port_option(args,"--port",0),[&](id c,bytes b,io_completion done){registry.handle(c,std::move(b),std::move(done));});
        if(!server.valid()) {std::cerr<<server.setup_error()<<'\n';return 1;}
        std::cout<<"READY host port="<<server.port()<<" resource_bytes="<<size<<" trusted_lab=1\n"<<std::flush;
        auto* loop=g_main_loop_new(context,FALSE);g_main_loop_run(loop);g_main_loop_unref(loop);
    } CAPSTAN_CATCH_STD(e) {std::cerr<<e.what()<<'\n';return 1;}
    return 0;
}
