#define main capstan_host_application_main
#include "src/apps/host.cpp"
#undef main
#include "tests/check.hpp"
#include <future>
int main() {
    auto* context=g_main_context_new();
    {
        app::config c{{1,2,3},7,{}};for(unsigned i=0;i<3;++i) c.credentials[i][0]=static_cast<unsigned char>(i+10);
        host_runtime host(c,context,128*1024,0,32);host.trace_requests=false;host.inline_version=false;
        grant g;g.key=c.key;g.epoch=1;g.grant_id=1;g.holder=1;g.mode=access_mode::exclusive;g.secret[0]=1;
        CHECK(host.authority.set_authority({c.key,7,1,0,1,false,{g}})==status::ok);
        gio_server server(context,0,[&](id connection,bytes payload,io_completion done) {host.handle(connection,std::move(payload),std::move(done));});
        CHECK(server.valid());
        gio_client business1(context,"127.0.0.1",server.port()),business2(context,"127.0.0.1",server.port());
        auto control=std::unique_ptr<gio_client>(new gio_client(context,"127.0.0.1",server.port()));
        CHECK(business1.valid() && business2.valid() && control->valid());
        unsigned business_slot=0;
        auto send=[&](unsigned role,bytes payload,io_completion done) {
            auto packet=wire::command(9999,{wire::value::string(app::token_bytes(c.credentials[role])),wire::value::string(wire::binary(payload))});
            auto& socket=role==2 ? *control : ((business_slot++%2)==0 ? business1 : business2);
            CHECK(socket.submit(std::move(packet),std::move(done)));
        };
        auto pump=[&](auto predicate) {
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!predicate()) {
                while(g_main_context_iteration(context,FALSE)) {}
                CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        migration::ticket t;t.control=7;t.sequence=1;t.source_host=1;t.source_instance=2;t.target_host=2;t.target_instance=3;t.generation=1;t.source=g;
        auto freeze=wire::command(migration::freeze_command,{wire::value::string(migration::encode_ticket(t))});
        unsigned denied=0;
        send(0,freeze,[&](io_result r) {CHECK(wire::decode_reply(r.payload).code==status::wrong_control);++denied;});
        pump([&]{return denied==1;});
        auto wrong=t;++wrong.source_instance;
        send(2,wire::command(migration::freeze_command,{wire::value::string(migration::encode_ticket(wrong))}),[&](io_result r) {
            CHECK(wire::decode_reply(r.payload).code==status::wrong_object);++denied;
        });pump([&]{return denied==2;});
        std::promise<void> entered,release;auto gate=release.get_future().share();auto started=entered.get_future();
        CHECK(host.worker.try_dispatch(flux_foundry::task_wrapper_sbo([&entered,gate]() noexcept {entered.set_value();gate.wait();})));
        CHECK(started.wait_for(std::chrono::seconds(5))==std::future_status::ready);
        unsigned completed=0,acks=0,unknown=0;
        send(0,wire::encode_call({g,41,method::add_range},{wire::u(0),wire::u(1),wire::u(9)}),[&](io_result r) {
            CHECK(wire::decode_reply(r.payload).code==status::ok);++completed;
        });
        send(0,wire::encode_call({g,42,method::version}),[&](io_result r) {
            CHECK(wire::decode_reply(r.payload).code==status::ok);++completed;
        });
        pump([&]{return host.authority.inspect().active==2;});
        auto frozen_reply=[&](io_result r) {
            if(r.state==outcome::unknown) {++unknown;return;}
            auto response=wire::decode_reply(r.payload);CHECK(response.code==status::ok && response.values.size()==2);
            CHECK(wire::number(response.values[1])==static_cast<unsigned>(migration::authority_phase::frozen));
            CHECK(host.authority.inspect().active==0);++acks;
        };
        send(2,freeze,frozen_reply);
        pump([&]{return host.authority.inspect().gate==host_authority::phase::closing;});
        CHECK(host.authority.inspect().active==2 && !acks);
        control->close();pump([&]{return unknown==1;});
        CHECK(host.authority.inspect().active==2 && host.authority.inspect().gate==host_authority::phase::closing);
        control.reset(new gio_client(context,"127.0.0.1",server.port()));CHECK(control->valid());
        send(2,freeze,frozen_reply); // Same immutable transaction after lost observation.
        release.set_value();auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(completed!=2 || acks!=1) {
            while(g_main_context_iteration(context,FALSE)) {}
            CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        migration::authority_image image;CHECK(host.authority.export_authority(t,image)==status::ok && image.requests.size()==2);
        migration::manifest manifest;bool exported=false;
        send(2,wire::command(migration::export_command,{wire::value::string(migration::encode_ticket(t))}),[&](io_result r) {
            auto response=wire::decode_reply(r.payload);CHECK(response.code==status::ok && response.values.size()==1);
            CHECK(migration::decode_manifest(wire::string(response.values[0]),manifest));exported=true;
        });
        pump([&]{return exported;});CHECK(manifest.size>migration::transfer_receiver::chunk_bytes);
        migration::transfer_receiver receiver;CHECK(receiver.begin(manifest)==status::ok);
        for(std::uint64_t offset=0;offset<manifest.size;offset+=migration::transfer_receiver::chunk_bytes) {
            bool received=false;
            send(2,wire::command(migration::export_chunk_command,{wire::value::string(migration::encode_ticket(t)),wire::u(offset)}),[&](io_result r) {
                auto response=wire::decode_reply(r.payload);CHECK(response.code==status::ok && response.values.size()==2);
                CHECK(wire::number(response.values[0])==offset);
                CHECK(receiver.append(manifest,offset,wire::binary(wire::string(response.values[1])))==status::ok);received=true;
            });pump([&]{return received;});
        }
        CHECK(receiver.seal(manifest,[](const migration::buffer& data,const std::array<std::uint8_t,32>& digest) noexcept {
            return migration::sha256(data)==digest ? status::ok : status::invalid;
        })==status::ok);
        auto retained=receiver.prepared_data();migration::reader package(*retained);std::uint64_t magic,schema,length;
        CHECK(package.get(4,magic) && magic==0x474d5043u && package.get(4,schema) && schema==1);
        std::string data,history;
        CHECK(package.get(8,length) && package.text(static_cast<std::size_t>(length),data));
        CHECK(package.get(8,length) && package.text(static_cast<std::size_t>(length),history) && !package.remaining());
        std::unique_ptr<dynabridge::capstan_native::dataset> restored;
        CHECK(migration::snapshot_traits<dynabridge::capstan_native::dataset>::restore(wire::binary(data),restored)==status::ok);
        dynabridge::capstan_native::dataset expected(128*1024,7);expected.add_range(0,1,9);
        auto expected_summary=expected.summary(),actual=restored->summary();
        CHECK(expected_summary.version==actual.version && expected_summary.sum==actual.sum && expected_summary.checksum==actual.checksum && expected_summary.count==actual.count);
        migration::authority_image remote_history;
        CHECK(migration::decode_authority_image(wire::binary(history),remote_history)==status::ok && remote_history==image);
        send(2,wire::command(migration::fence_command,{wire::value::string(migration::encode_ticket(t))}),[&](io_result r) {CHECK(wire::decode_reply(r.payload).code==status::ok);++acks;});
        pump([&]{return acks==2;});
        unsigned released=0;
        auto release_packet=wire::command(migration::export_release_command,{wire::value::string(migration::encode_ticket(t))});
        for(unsigned retry=0;retry<2;++retry) {
            send(2,release_packet,[&](io_result r) {CHECK(wire::decode_reply(r.payload).code==status::ok);++released;});
            pump([&]{return released==retry+1;});
        }
        CHECK(host.exports.empty() && host.export_reserved==0);
        CHECK(retained->size()==manifest.size); // Receiver owns its own completed data.
        CHECK(unknown==1 && acks==2 && host.authority.admit({g,43,method::version},1).code==status::closed);
    }
    while(g_main_context_iteration(context,FALSE)) {}g_main_context_unref(context);
    std::cout<<"Authenticated TCP Host migration controls await actual worker Drain\n";
}
