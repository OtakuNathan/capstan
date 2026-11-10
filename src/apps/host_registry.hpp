#pragma once
// Storage policy follows each runtime, including dynamically restored objects.
template<class Runtime>
struct basic_host_registry {
    using host_runtime=Runtime;
    using hooks=typename Runtime::hooks;
    using object_type=typename Runtime::object_type;
    using key_type=std::array<id,3>;
    static key_type key(object_key k) noexcept {return {{k.cluster,k.incarnation,k.object}};}
    host_runtime& root;
    // C++14 ordinary new need not honor the FF worker's extended alignment.
    // Retain an owning byte allocation and destroy the aligned placement object
    // before releasing it; no C++17 aligned-new dependency is required.
    struct runtime_deleter {
        unsigned char* allocation;
        explicit runtime_deleter(unsigned char* data=nullptr) noexcept:allocation(data) {}
        void operator()(host_runtime* value) const noexcept {if(value) value->~host_runtime();delete[] allocation;}
    };
    using runtime_owner=std::unique_ptr<host_runtime,runtime_deleter>;
    struct object_slot {runtime_owner runtime;bool published=false;};
    std::map<key_type,object_slot> objects;
    enum class clone_phase {receiving,installed,aborted};
    struct clone_record {
        id holder=0,sequence=0;
        migration::snapshot_ref source;
        object_key target;
        std::string address;unsigned short port=0;
        grant installed;
        clone_phase phase=clone_phase::receiving;
        std::size_t reserved=0;
        bool pulling=false;
        migration::manifest manifest;
        std::shared_ptr<migration::transfer_receiver> receiver;
    };
    std::map<std::pair<id,id>,std::shared_ptr<clone_record>> clones;
    std::size_t clone_reserved=0;
    explicit basic_host_registry(host_runtime& runtime):root(runtime) {root.other_usage=[this]{return other_bytes(&root);};}
    ~basic_host_registry() {root.other_usage=nullptr;}
    static std::size_t usage(const host_runtime& r) noexcept {
        return (r.owner ? r.owner->resource_bytes() : 0)+r.export_reserved+r.import_reserved+r.copy_reserved;
    }
    std::size_t other_bytes(const host_runtime* self) const noexcept {
        std::size_t total=clone_reserved+(self!=&root ? usage(root) : 0);
        for(const auto& item:objects) if(item.second.runtime.get()!=self) total+=usage(*item.second.runtime);
        return total;
    }
    runtime_owner empty_runtime(object_key k) {
        auto config=root.config;config.key=k;
        const auto bytes=sizeof(host_runtime)+alignof(host_runtime)-1;
        auto allocation=std::unique_ptr<unsigned char[]>(new unsigned char[bytes]);
        void* aligned=allocation.get();auto space=bytes;
        if(!std::align(alignof(host_runtime),sizeof(host_runtime),aligned,space)) std::terminate();
        auto* created=new(aligned) host_runtime(config,root.context,0,0,root.history_limit
#ifdef CAPSTAN_BENCHMARK_BUILD
            ,host_authority::benchmark_validation::checked
#endif
            ,root.backend);
        runtime_owner runtime(created,runtime_deleter{allocation.release()});
        if(!runtime->worker.valid()) return runtime_owner(nullptr,runtime_deleter{});
        runtime->host_id=root.host_id;runtime->host_instance=root.host_instance;runtime->export_budget=root.export_budget;
        runtime->trace_migration_metrics=root.trace_migration_metrics;runtime->trace_requests=root.trace_requests;runtime->inline_version=root.inline_version;
        auto* pointer=runtime.get();runtime->other_usage=[this,pointer]{return other_bytes(pointer);};return runtime;
    }
    void receipt(const clone_record& record,io_completion done,status code=status::ok) noexcept {
        CAPSTAN_TRY {
        std::vector<wire::value> values{wire::u(static_cast<unsigned>(record.phase))};
        if(record.phase==clone_phase::installed) values.push_back(wire::value::string(wire::encode_grant(record.installed)));
        app::reply(std::move(done),code,std::move(values));
        } CAPSTAN_CATCH_ALL {if(done) {CAPSTAN_TRY {done({outcome::unknown,{}});} CAPSTAN_CATCH_ALL {}}}
    }
    void pull(const std::shared_ptr<clone_record>& record,io_completion done) {
        if(record->phase==clone_phase::installed) {receipt(*record,std::move(done));return;}
        if(record->phase==clone_phase::aborted) {receipt(*record,std::move(done),status::closed);return;}
        if(record->pulling) {receipt(*record,std::move(done),status::pending);return;}
        auto peer=std::make_shared<gio_client>(root.context,record->address,record->port,app::transfer_limits());
        if(!peer->valid()) {receipt(*record,std::move(done),status::rejected);return;}
        auto callback=std::make_shared<io_completion>(std::move(done));
        auto transfer=std::make_shared<app::snapshot_pull>(peer,root.config.credentials[record->holder-1],record->source,record->manifest,record->receiver,
            [this,record,callback](status code) noexcept {
                CAPSTAN_TRY {
                    if(record->phase==clone_phase::aborted) code=status::closed;
                    if(code==status::ok) code=record->receiver->seal(record->manifest,[&](const migration::buffer& bytes,const std::array<std::uint8_t,32>& hash) noexcept {
                        CAPSTAN_TRY {
                            (void)hash; // Receiver verified the ordered incremental SHA-256 before Seal.
                                if(!record->receiver->digest_valid()) return status::invalid;
                            auto& target=*objects.at(key(record->target)).runtime;
                            std::unique_ptr<object_type> binding;
                            auto result=target.backend->restore(record->target,{bytes.data(),bytes.size()},binding);
                            if(result!=status::ok) return result;
                            if(!binding || binding->resource_bytes()>target.backend->restore_reservation(bytes.size())) return status::exhausted;
                            // Fresh logical object: fresh authority and an empty request
                            // history. Copying source authority would make a clone a replay.
                            transition install{record->target,root.config.control,record->sequence,0,1,false,{record->installed}};
                            result=target.authority.set_authority(install);
                            if(result==status::ok) target.owner=std::move(binding);
                            return result;
                        } CAPSTAN_CATCH_ALL {return status::exhausted;}
                    });
                    if(code==status::ok) {
                        objects.at(key(record->target)).published=true;record->phase=clone_phase::installed;
                        record->receiver->release_prepared_storage();record->receiver.reset();clone_reserved-=record->reserved;record->reserved=0;
                    }
                } CAPSTAN_CATCH_ALL {code=status::exhausted;}
                record->pulling=false;
                auto done=std::exchange(*callback,nullptr);
                CAPSTAN_TRY {receipt(*record,std::move(done),code);}
                CAPSTAN_CATCH_ALL {if(done) done({outcome::unknown,{}});}
            });
        record->pulling=true;transfer->start();
    }
    void handle_clone(id holder,const wire::codec::request& request,io_completion done) {
        if(holder!=1 && holder!=2) {app::reply(std::move(done),status::forbidden);return;}
        const auto& a=request.args;
        if(a.empty() || !wire::is_number(a[0]) || !wire::number(a[0])) {app::reply(std::move(done),status::invalid);return;}
        const auto sequence=wire::number(a[0]);auto identity=std::make_pair(holder,sequence);
        auto found=clones.find(identity);
        if(request.method!=migration::clone_command) {
            if(a.size()!=1) {app::reply(std::move(done),status::invalid);return;}
            if(found==clones.end()) {
                if(request.method==migration::clone_query_command) {app::reply(std::move(done),status::not_seen);return;}
                if(clones.size()>=64) {app::reply(std::move(done),status::exhausted);return;}
                auto tombstone=std::make_shared<clone_record>();tombstone->holder=holder;tombstone->sequence=sequence;tombstone->phase=clone_phase::aborted;
                found=clones.emplace(identity,tombstone).first;
            }
            auto& record=*found->second;
            if(request.method==migration::clone_abort_command) {
                if(record.phase==clone_phase::installed) {receipt(record,std::move(done),status::conflict);return;}
                record.phase=clone_phase::aborted;
                if(record.receiver) record.receiver->abort(record.manifest);
                clone_reserved-=record.reserved;record.reserved=0;
                // Retain the unpublished slot/tombstone; never reuse its identity.
            }
            receipt(record,std::move(done));return;
        }
        migration::snapshot_ref ref;
        if(a.size()!=7 || !wire::is_string(a[1]) || !migration::decode_snapshot_ref(wire::string(a[1]),ref) || ref.holder!=holder ||
           !wire::is_number(a[2]) || !wire::is_number(a[3]) || !wire::is_number(a[4]) || !wire::is_string(a[5]) ||
           !wire::is_number(a[6]) || !wire::number(a[6]) || wire::number(a[6])>65535) {app::reply(std::move(done),status::invalid);return;}
        object_key target{wire::number(a[2]),wire::number(a[3]),wire::number(a[4])};
        auto address=wire::string(a[5]);auto port=static_cast<unsigned short>(wire::number(a[6]));
        if(!target.cluster || !target.incarnation || !target.object || target==ref.source) {app::reply(std::move(done),status::invalid);return;}
        if(found!=clones.end()) {
            auto record=found->second;
            if(record->phase==clone_phase::aborted) {receipt(*record,std::move(done),status::closed);return;}
            if(!(record->source==ref) || record->target!=target || record->address!=address || record->port!=port) {app::reply(std::move(done),status::conflict);return;}
            pull(record,std::move(done));return;
        }
        if(target==root.config.key || objects.count(key(target))) {app::reply(std::move(done),status::conflict);return;}
        auto total=usage(root)+other_bytes(&root);
        if(clones.size()>=64 || objects.size()>=64 || ref.size>root.export_budget || total>root.export_budget) {app::reply(std::move(done),status::exhausted);return;}
        const auto restored=root.backend->restore_reservation(static_cast<std::size_t>(ref.size));
        if(restored>root.export_budget-total || ref.size>root.export_budget-total-restored) {app::reply(std::move(done),status::exhausted);return;}
        auto record=std::make_shared<clone_record>();record->holder=holder;record->sequence=sequence;record->source=ref;record->target=target;
        record->address=std::move(address);record->port=port;record->reserved=static_cast<std::size_t>(ref.size)+restored;
        record->installed.key=target;record->installed.epoch=1;record->installed.holder=holder;record->installed.mode=access_mode::exclusive;
        if(!try_random_id(record->installed.grant_id) || !try_random_token(record->installed.secret)) {app::reply(std::move(done),status::exhausted);return;}
        record->manifest={{root.config.control,sequence},ref.source,ref.host,root.host_id,ref.instance,root.host_instance,1,ref.size,ref.digest};
        record->receiver=std::make_shared<migration::transfer_receiver>(ref.size,migration::make_sha256_stream());
        auto code=record->receiver->begin(record->manifest);if(code!=status::ok) {app::reply(std::move(done),code);return;}
        auto native=empty_runtime(target);if(!native) {app::reply(std::move(done),status::exhausted);return;}
        objects.emplace(key(target),object_slot{std::move(native),false});
        CAPSTAN_TRY {clones.emplace(identity,record);}
        CAPSTAN_CATCH_ALL {objects.erase(key(target));CAPSTAN_RETHROW;}
        clone_reserved+=record->reserved;pull(record,std::move(done));
    }
    void handle(id,bytes packet,io_completion done) {
        std::shared_ptr<io_completion> slot;
        CAPSTAN_TRY {
            app::authenticated auth;wire::codec::request request;
            if(!app::try_authenticate(root.config,packet,auth) || !wire::try_decode(auth.payload,request)) {app::reply(std::move(done),status::invalid);return;}
            if(!hooks::healthy(*root.backend)) {done({outcome::unknown,{}});return;}
            if(!hooks::allows(request.method)) {app::reply(std::move(done),status::forbidden);return;}
            if(request.method>=migration::clone_command && request.method<=migration::clone_abort_command) {
                slot=std::make_shared<io_completion>();*slot=std::move(done);
                handle_clone(auth.holder,request,[slot](io_result result) {auto done=std::exchange(*slot,nullptr);if(done) done(std::move(result));});return;
            }
            if(request.method!=migration::route_command) {root.handle_authenticated(std::move(auth),std::move(request),std::move(done));return;}
            const auto& a=request.args;
            if(a.size()!=4 || !wire::is_number(a[0]) || !wire::is_number(a[1]) || !wire::is_number(a[2]) || !wire::is_string(a[3])) {app::reply(std::move(done),status::invalid);return;}
            object_key target{wire::number(a[0]),wire::number(a[1]),wire::number(a[2])};
            auto inner=wire::binary(wire::string(a[3]));host_runtime* runtime=&root;
            wire::codec::request command;if(!wire::try_decode(inner,command)) {app::reply(std::move(done),status::invalid);return;}
            if(target!=root.config.key) {
                auto found=objects.find(key(target));
                if(found==objects.end()) {
                    migration::ticket t;
                    if(auth.holder!=3 || command.method!=migration::reserve_command || command.args.empty() ||
                       !wire::is_string(command.args[0]) || !migration::decode_ticket(wire::string(command.args[0]),t) || t.source.key!=target ||
                       t.target_host!=root.host_id || t.target_instance!=root.host_instance || t.control!=root.config.control) {app::reply(std::move(done),status::wrong_object);return;}
                    if(objects.size()>=64) {app::reply(std::move(done),status::exhausted);return;}
                    auto created=empty_runtime(target);if(!created) {app::reply(std::move(done),status::exhausted);return;}
                    found=objects.emplace(key(target),object_slot{std::move(created),true}).first;
                }
                if(!found->second.published) {app::reply(std::move(done),status::closed);return;}
                runtime=found->second.runtime.get();
            }
            auth.payload=std::move(inner);runtime->handle_authenticated(std::move(auth),std::move(command),std::move(done));
        } CAPSTAN_CATCH_ALL {
            if(slot) done=std::exchange(*slot,nullptr);
            app::reply(std::move(done),status::exhausted);
        }
    }
};
