#include "examples/owner/sqlite.hpp"
#include "owner_host.hpp"
using durable_host_runtime=basic_host_runtime<capstan::examples::sqlite_owner_backend>;
int main(int argc,char** argv) {
    CAPSTAN_TRY {
        auto args=app::options(argc,argv);auto config=app::load_config(app::required(args,"--config"));
        auto size=app::option_number(args,"--bytes",1024*1024);
        auto history=app::option_number(args,"--history-limit",4096);
        auto delay=app::option_number(args,"--work-delay-ms",0);
        if(!size || size>256*1024*1024 || !history || history>1048576 || delay>1000) app::startup_failure("persistent Host options out of bounds");
        auto backend=std::make_shared<examples::sqlite_owner_backend>(app::required(args,"--state-file"));
        auto* context=g_main_context_default();
        durable_host_runtime runtime(config,context,size,static_cast<unsigned>(delay),static_cast<std::size_t>(history),backend);
        runtime.trace_requests=app::option_number(args,"--trace-requests",1)!=0;
        if(!runtime.worker.valid()) app::startup_failure("business worker startup failed");
        gio_server server(context,app::port_option(args,"--port",0),[&](id c,bytes b,io_completion done){runtime.handle(c,std::move(b),std::move(done));});
        if(!server.valid()) app::startup_failure(server.setup_error().c_str());
        std::cout<<"READY durable-host port="<<server.port()<<" resource_bytes="<<runtime.owner->resource_bytes()<<" trusted_lab=1\n"<<std::flush;
        auto* loop=g_main_loop_new(context,FALSE);g_main_loop_run(loop);g_main_loop_unref(loop);
    } CAPSTAN_CATCH_STD(e) {std::cerr<<e.what()<<'\n';return 1;}
    return 0;
}
