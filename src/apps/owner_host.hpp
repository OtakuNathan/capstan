#pragma once
#include <capstan/error_policy.hpp>
#include "common.hpp"
#include "worker.hpp"
#include "authority_replies.hpp"
#include "migration_replies.hpp"
#include "migration_metrics.hpp"
#include <capstan/migration/package.hpp>
#include <capstan/migration/clipboard.hpp>
#include "migration_peer.hpp"
#include <capstan/migration/wire.hpp>
#include <capstan/migration/gio_digest.hpp>
#include <capstan/authority/host.hpp>
#include <capstan/protocol/random.hpp>
#include <capstan/owner/runtime_hooks.hpp>
#include <chrono>
#include <thread>
using namespace capstan;
template<class Backend>
struct basic_host_runtime {
    using hooks=owner_runtime_contract<Backend>;
    using backend_contract=owner_backend_contract<Backend>;
    using object_type=typename backend_contract::object_type;
    // Factory/context outlives active and unpublished restored objects.
    std::shared_ptr<Backend> backend;
    app::config config;
    GMainContext* context;
    host_authority authority;
    app::authority_replies control_replies{authority};
    app::migration_replies migration_replies{authority};
    id host_id=1,host_instance;
    struct export_record {
        migration::ticket ticket;
        std::shared_ptr<migration::buffer> payload;
        migration::manifest manifest;
        status code=status::pending;
        bool started=false,capture_ready=false,prefetching=false;
        std::size_t reserved=0,limit=0;
    };
    std::map<id,std::shared_ptr<export_record>> exports;
    std::size_t export_reserved=0;
    const std::size_t history_limit;
    std::size_t export_budget=512u*1024u*1024u;
    enum class import_phase {reserved,receiving,prepared,active,aborted};
    struct import_record {
        migration::ticket ticket;
        grant successor;
        import_phase phase=import_phase::reserved;
        std::size_t limit=0,reserved=0;
        bool pulling=false;
        std::shared_ptr<gio_client> prepared_peer;
        std::string peer_address;unsigned short peer_port=0;
        std::shared_ptr<migration::transfer_receiver> receiver;
        std::shared_ptr<app::transfer_metrics> metrics;
        std::unique_ptr<object_type> restored;
        migration::manifest manifest;
    };
    std::map<id,std::shared_ptr<import_record>> imports;
    std::size_t import_reserved=0;
    std::unique_ptr<object_type> owner;
    app::business_worker worker;
    app::snapshot_worker capture_worker;
    struct copy_record {
        grant source;migration::snapshot_ref reference;
        std::shared_ptr<migration::buffer> payload;
        status code=status::pending;bool released=false;
        std::size_t reserved=0;
    };
    std::map<std::pair<id,id>,std::shared_ptr<copy_record>> copies;
    std::size_t copy_reserved=0;
    std::function<std::size_t()> other_usage;
    std::size_t other_bytes() const {return copy_reserved+(other_usage ? other_usage() : 0);}
    struct pending { wire::call call; io_completion done; std::shared_ptr<copy_record> copy; };
    std::map<id,std::shared_ptr<pending>> operations;
    unsigned delay_ms;
    bool trace_requests=true,trace_migration_metrics=false;
    id metric_sequence=0;
    app::metric_stamp freeze_metric,export_ready_metric;
    bool business_drain_reported=false,barrier_reported=false;
    bool inline_version=true;
    id admitted=0,completed=0,rejected=0,control_calls=0,destroyed=0;
    basic_host_runtime(app::config c,GMainContext* ctx,std::size_t size,unsigned delay,std::size_t history_limit
#ifdef CAPSTAN_BENCHMARK_BUILD
        ,host_authority::benchmark_validation validation
#endif
        ,std::shared_ptr<Backend> storage=std::make_shared<Backend>())
        :backend(require_owner_backend(std::move(storage))),config(c),context(ctx),authority(c.key,c.control
#ifdef CAPSTAN_BENCHMARK_BUILD
        ,validation
#endif
        ,64,history_limit),host_instance(c.key.incarnation),history_limit(history_limit),owner(size ? backend->create(c.key,size,7) : nullptr),delay_ms(delay) {
        if(size && !owner) CAPSTAN_RAISE(std::runtime_error("owner backend creation failed"));
        if(!hooks::initialize(*backend,authority,c.control,history_limit)) CAPSTAN_RAISE(std::runtime_error("owner recovery failed"));
    }
    ~basic_host_runtime() {capture_worker.close();worker.close();}
    bool collect_resource() {
        if(!hooks::checkpoint(*backend,authority)) {
            control_replies.fail_unknown();
            // Queued work has never accessed the backend. Executing work keeps
            // its real pin until its worker completion, even after storage fault.
            const auto executing=authority.inspect().executing;
            auto it=operations.begin();
            while(it!=operations.end()) {
                if(it->first==executing) {++it;continue;}
                auto pin=it->first;auto frame=it->second;it=operations.erase(it);
                authority.dispatch_failed(pin);auto done=std::exchange(frame->done,nullptr);
                if(done) done({outcome::unknown,{}});
            }
            return false;
        }
        if(trace_migration_metrics && freeze_metric.wall && !barrier_reported) {
            const auto occupancy=authority.inspect_occupancy();
            if(!occupancy.readers && !occupancy.writers) {
                if(!business_drain_reported) {app::emit_metric("source_business_drain",freeze_metric,app::metric_stamp::now());business_drain_reported=true;}
                if(!occupancy.captures) {app::emit_metric("source_full_barrier",freeze_metric,app::metric_stamp::now());barrier_reported=true;}
            }
        }
        if(authority.inspect().gate==host_authority::phase::dead && owner) {
            owner.reset(); ++destroyed; std::cout<<"DESTROY active=0\n"<<std::flush;
        }
        control_replies.flush();
        migration_replies.flush();return true;
    }
    void kick() {
        host_authority::operation op;
        if(!hooks::healthy(*backend) || operations.empty() || authority.inspect().executing) return;
        auto pin=operations.begin()->first;
        auto frame=operations.begin()->second;
        // Construct the dispatch task before starting the operation. If task
        // allocation fails, pin remains queued; no unpinned native execution.
        CAPSTAN_TRY {
        auto task=flux_foundry::task_wrapper_sbo([this,pin,frame]() noexcept {
            bytes response;status copy_code=status::invalid;
            CAPSTAN_TRY {
                if(delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                if(frame->copy) {
                    auto& record=*frame->copy;
                    copy_code=owner->capture_snapshot(*record.payload);
                    if(copy_code==status::ok && (!record.payload->size() || record.payload->size()!=owner->snapshot_bytes())) copy_code=status::invalid;
                    if(copy_code==status::ok && record.payload->capacity()>record.reserved) copy_code=status::exhausted;
                    if(copy_code==status::ok) {record.reference.size=record.payload->size();record.reference.digest=migration::sha256(*record.payload);}
                    response=wire::encode_reply(copy_code==status::ok ? outcome::completed : outcome::rejected,copy_code,
                        copy_code==status::ok ? std::vector<wire::value>{wire::value::string(migration::encode_snapshot_ref(record.reference))} : std::vector<wire::value>{});
                } else {
                response=hooks::execute(*backend,*owner,frame->call);
                }
            } CAPSTAN_CATCH_ALL { response=wire::encode_reply(hooks::durable ? outcome::unknown : outcome::completed,status::invalid); }
            app::post(context,[this,pin,copy_code,response=std::move(response)]() mutable {
                auto it=operations.find(pin);
                if(it->second->copy) {
                    auto& record=*it->second->copy;record.code=copy_code;
                    auto retained=copy_code==status::ok ? record.payload->capacity() : 0;
                    copy_reserved-=record.reserved-retained;record.reserved=retained;
                    if(copy_code!=status::ok) record.payload.reset();
                } auto done=std::exchange(it->second->done,nullptr); operations.erase(it);
                if(authority.complete(pin)!=status::ok) std::terminate();
                ++completed;
                if(trace_requests) std::cout<<"COMPLETE pin="<<pin<<" active="<<authority.inspect().active<<'\n'<<std::flush;
                collect_resource(); kick(); done({outcome::completed,std::move(response)});
            });
        });
        if(!authority.start_next(op)) return;
        // The fixed version getter is bounded and cannot block. Keep the same
        // admission pin, FIFO lane and posted completion, but avoid waking a
        // worker for this scalar read. Scans, writes and injected-delay tests
        // continue on the worker. An already executing operation still wins.
        if(inline_version && backend->can_inline(op.call.operation) && !delay_ms && op.call.operation==method::version) {
            task();return;
        }
        if(!worker.try_dispatch(std::move(task))) {
            // start_next already marked this pin executing. A false submission
            // guarantees the task never ran and cannot later run; end the pin
            // here, just as an operation terminating before business access.
            if(authority.complete(pin)!=status::ok) std::terminate();
            auto done=std::exchange(frame->done,nullptr);operations.erase(pin);++rejected;
            fail_copy(frame);collect_resource();app::reply(std::move(done),status::rejected);kick();
        }
        } CAPSTAN_CATCH_ALL {
            authority.dispatch_failed(pin);
            auto done=std::exchange(frame->done,nullptr);operations.erase(pin);++rejected;
            fail_copy(frame);collect_resource();app::reply(std::move(done),status::rejected);kick();
        }
    }
    void fail_copy(const std::shared_ptr<pending>& frame) noexcept {
        if(frame->copy) {auto& r=*frame->copy;r.code=status::rejected;r.payload.reset();copy_reserved-=r.reserved;r.reserved=0;}
    }
    std::size_t export_limit() const noexcept {
        // Upper bound for the three bounded history maps plus Dataset framing.
        const auto bytes=owner ? owner->snapshot_bytes() : 0;
        if(bytes>export_budget) return export_budget+1;
        return bytes+history_limit*2048+4096;
    }
    status reserve_source(const migration::ticket& t,std::size_t limit) {
        auto found=exports.find(t.sequence);
        if(found!=exports.end()) return found->second->ticket==t && found->second->limit==limit ? status::ok : status::conflict;
        if(!owner) return status::dead;
        auto current=authority.inspect();
        if(current.epoch!=t.source.epoch || authority.placement_generation()!=t.generation || current.grants!=std::vector<grant>{t.source}) return status::stale;
        migration::authority_phase phase;
        auto seen=authority.migration_status(t,phase);
        if(current.gate!=host_authority::phase::open && !(seen==status::ok && phase==migration::authority_phase::frozen)) return status::closed;
        if(limit<export_limit() || limit>export_budget/2 || exports.size()>=64 || owner->resource_bytes()>export_budget ||
           2*limit>export_budget-owner->resource_bytes() || export_reserved+import_reserved+other_bytes()>export_budget-owner->resource_bytes()-2*limit) return status::exhausted;
        auto record=std::make_shared<export_record>();record->ticket=t;record->limit=limit;record->reserved=2*limit;
        record->payload=std::make_shared<migration::buffer>();record->payload->reserve(limit);
        exports.emplace(t.sequence,record);export_reserved+=record->reserved;return status::ok;
    }
    status capture_package_prefix(export_record& record) {
        // Same CPMG bytes as before, without a separate full Dataset buffer.
        // This storage stays unpublished until the final image and hash exist.
        auto& package=*record.payload;package.clear();
        migration::put(package,0x474d5043u,4);migration::put(package,1,4);
        const auto expected=owner->snapshot_bytes();
        if(!expected || expected>record.limit || record.limit-expected<16) return status::exhausted;
        migration::put(package,expected,8);
        const auto code=owner->append_snapshot(package);
        if(code!=status::ok) return code;
        return package.size()==expected+16 ? status::ok : status::invalid;
    }
    void prefetch_read_snapshot(const migration::ticket& t) noexcept {
        if(!Backend::concurrent_read_capture) return;
        // Optional optimization: all failures fall back to capture after Drain.
        // The admission mutex checks every queued/executing effect, not the
        // grant's exclusive/shared label. Native Dataset const reads are safe.
        CAPSTAN_TRY {
            auto found=exports.find(t.sequence);
            if(found==exports.end() || !(found->second->ticket==t) || !authority.inspect().active) return;
            auto record=found->second;
            if(record->started || record->prefetching || record->capture_ready) return;
            auto& lane=capture_worker.get();
            if(!lane.valid()) return;
            auto capture=authority.begin_migration_capture(t,true);
            if(capture.code!=status::ok) return;
            bool accepted=false;
            CAPSTAN_TRY {
                auto task=flux_foundry::task_wrapper_sbo([this,record,pin=capture.pin]() noexcept {
                    const auto started=app::metric_stamp::now(trace_migration_metrics);
                    status code=status::invalid;
                    CAPSTAN_TRY {code=capture_package_prefix(*record);} CAPSTAN_CATCH_ALL {code=status::exhausted;}
                    const auto finished=app::metric_stamp::now(trace_migration_metrics);
                    app::post(context,[this,record,pin,code,started,finished] {
                        app::emit_metric("source_prefetch",started,finished);
                        record->prefetching=false;record->capture_ready=code==status::ok;
                        if(code!=status::ok) record->payload->clear();
                        if(authority.complete(pin)!=status::ok) std::terminate();
                        migration::authority_phase phase;
                        if(authority.migration_status(record->ticket,phase)!=status::ok || (phase!=migration::authority_phase::draining && phase!=migration::authority_phase::frozen)) {
                            record->capture_ready=false;record->payload->clear();
                        }
                        collect_resource();
                    });
                });
                record->prefetching=true;accepted=lane.try_dispatch(std::move(task));
            } CAPSTAN_CATCH_ALL {}
            if(!accepted) {record->prefetching=false;if(authority.complete(capture.pin)!=status::ok) std::terminate();}
        } CAPSTAN_CATCH_ALL {}
    }
    void export_snapshot(const migration::ticket& ticket,io_completion done) noexcept {
        std::shared_ptr<io_completion> completion;
        CAPSTAN_TRY {
        auto found=exports.find(ticket.sequence);
        if(found!=exports.end() && !(found->second->ticket==ticket)) {app::reply(std::move(done),status::conflict);return;}
        if(found!=exports.end() && found->second->started) {
            const auto& r=*found->second;
            app::reply(std::move(done),r.code,r.code==status::ok ? std::vector<wire::value>{wire::value::string(migration::encode_manifest(r.manifest))} : std::vector<wire::value>{});return;
        }
        if(!owner) {app::reply(std::move(done),status::dead);return;}
        migration::authority_image authority_image;
        auto code=authority.export_authority(ticket,authority_image);
        if(code!=status::ok) {app::reply(std::move(done),code);return;}
        if(found==exports.end()) {
            code=reserve_source(ticket,export_limit());
            if(code!=status::ok) {app::reply(std::move(done),code);return;}
            found=exports.find(ticket.sequence);
        }
        auto record=found->second;
        migration::buffer image;code=migration::encode_authority_image(authority_image,image);
        if(code!=status::ok) {app::reply(std::move(done),code);return;}
        if(owner->snapshot_bytes()+image.size()+24>record->limit) {app::reply(std::move(done),status::exhausted);return;}
        const auto reserve=record->reserved;
        // Allocate frame/closure before pinning or accepting backend work.
        completion=std::make_shared<io_completion>(std::move(done));
        auto capture=authority.begin_migration_capture(ticket);
        if(capture.code!=status::ok) {app::reply(std::exchange(*completion,nullptr),capture.code);return;}
        bool registered=false;
        CAPSTAN_TRY {
            record->started=true;registered=true;
            auto task=flux_foundry::task_wrapper_sbo([this,record,completion,pin=capture.pin,reserve,image=std::move(image)]() mutable noexcept {
                const auto encoding_started=app::metric_stamp::now(trace_migration_metrics);
                app::metric_stamp hashing_started,hashing_finished;
                status code=status::invalid;
                CAPSTAN_TRY {
                    if(record->capture_ready) {record->capture_ready=false;code=status::ok;}
                    else code=capture_package_prefix(*record);
                    if(code==status::ok) {
                        auto& package=*record->payload;
                        migration::put(package,image.size(),8);package.insert(package.end(),image.begin(),image.end());
                        const auto& t=record->ticket;auto& m=record->manifest;
                        m.transaction={t.control,t.sequence};m.object=t.source.key;m.source_host=t.source_host;m.target_host=t.target_host;
                        m.source_instance=t.source_instance;m.target_instance=t.target_instance;m.generation=t.generation;
                        m.size=package.size();hashing_started=app::metric_stamp::now(trace_migration_metrics);
                        m.digest=migration::sha256(package);hashing_finished=app::metric_stamp::now(trace_migration_metrics);
                    }
                } CAPSTAN_CATCH_ALL {code=status::exhausted;}
                migration::buffer().swap(image); // Release capture temporaries before publishing completion.
                const auto encoding_finished=app::metric_stamp::now(trace_migration_metrics);
                app::post(context,[this,record,completion,pin,reserve,code,encoding_started,encoding_finished,hashing_started,hashing_finished]() mutable {
                    app::emit_metric("source_encode_and_hash",encoding_started,encoding_finished);
                    app::emit_metric("source_hash",hashing_started,hashing_finished);
                    if(authority.complete(pin)!=status::ok) std::terminate();
                    migration::authority_phase phase;
                    if(code==status::ok && record->payload->capacity()>reserve) code=status::exhausted;
                    if(authority.migration_status(record->ticket,phase)!=status::ok || phase!=migration::authority_phase::frozen) code=status::closed;
                    record->code=code;
                    if(code==status::ok) export_ready_metric=app::metric_stamp::now(trace_migration_metrics);
                    auto actual=code==status::ok ? record->payload->capacity() : 0;
                    record->reserved=actual;
                    export_reserved-=reserve-actual;
                    if(code!=status::ok) record->payload.reset();
                    collect_resource();
                    io_result result{outcome::unknown,{}};
                    CAPSTAN_TRY {
                        auto values=code==status::ok ? std::vector<wire::value>{wire::value::string(migration::encode_manifest(record->manifest))} : std::vector<wire::value>{};
                        result={outcome::completed,wire::encode_reply(code==status::ok ? outcome::completed : outcome::rejected,code,values)};
                    } CAPSTAN_CATCH_ALL {}
                    auto done=std::exchange(*completion,nullptr);
                    if(done) {CAPSTAN_TRY {done(std::move(result));} CAPSTAN_CATCH_ALL {}}
                });
            });
            if(worker.try_dispatch(std::move(task))) return;
        } CAPSTAN_CATCH_ALL {}
        if(authority.complete(capture.pin)!=status::ok) std::terminate();
        if(registered) record->started=false;
        collect_resource();app::reply(std::exchange(*completion,nullptr),status::exhausted);
        } CAPSTAN_CATCH_ALL {
            if(completion) done=std::exchange(*completion,nullptr);
            if(done) {CAPSTAN_TRY {done({outcome::unknown,{}});} CAPSTAN_CATCH_ALL {}}
        }
    }
    status reserve_destination(const migration::ticket& t,const grant& successor,std::uint64_t limit) {
        auto old=imports.find(t.sequence);
        if(old!=imports.end()) {
            const auto& r=*old->second;
            if(!(r.ticket==t) || !(r.successor==successor) || r.limit!=limit) return status::conflict;
            return r.phase==import_phase::aborted ? status::closed : status::ok;
        }
        if(successor.key!=t.source.key || successor.epoch!=t.source.epoch+1 || successor.holder!=t.source.holder ||
           successor.mode!=access_mode::exclusive || !successor.grant_id || successor.secret==token{} || successor.secret==t.source.secret) return status::invalid;
        const auto native=owner ? owner->resource_bytes() : 0;
        if(imports.size()>=64 || !limit || limit>export_budget || native>export_budget) return status::exhausted;
        const auto restored=backend->restore_reservation(static_cast<std::size_t>(limit));
        if(restored>export_budget-native || limit>export_budget-native-restored) return status::exhausted;
        const auto reserve=static_cast<std::size_t>(limit)+restored;
        if(export_reserved+import_reserved+other_bytes()>export_budget-native-reserve) return status::exhausted;
        auto record=std::make_shared<import_record>();record->ticket=t;record->successor=successor;
        record->limit=static_cast<std::size_t>(limit);record->reserved=reserve;
        std::unique_ptr<migration::transfer_digest> digest;
        if(trace_migration_metrics) {record->metrics=std::make_shared<app::transfer_metrics>();digest.reset(new app::measured_digest(record->metrics));}
        else digest=migration::make_sha256_stream();
        record->receiver=std::make_shared<migration::transfer_receiver>(record->limit,std::move(digest));
        // Allocate map entry first; a failed reservation cannot leave an
        // untracked destination gate or partially published native object.
        imports.emplace(t.sequence,record);
        status code;
        CAPSTAN_TRY {code=authority.reserve_import(t);}
        CAPSTAN_CATCH_ALL {imports.erase(t.sequence);CAPSTAN_RETHROW;}
        if(code!=status::ok) {imports.erase(t.sequence);return code;}
        import_reserved+=record->reserved;return status::ok;
    }
    void pull_destination(const std::shared_ptr<import_record>& record,const std::string& address,unsigned short port,io_completion done) {
        if(record->phase==import_phase::aborted) {app::reply(std::move(done),status::closed);return;}
        if(record->phase==import_phase::prepared || record->phase==import_phase::active) {app::reply(std::move(done),status::ok);return;}
        if(record->pulling) {app::reply(std::move(done),status::pending);return;}
        if(record->peer_port && (record->peer_address!=address || record->peer_port!=port)) {app::reply(std::move(done),status::conflict);return;}
        auto peer=record->prepared_peer ? std::exchange(record->prepared_peer,nullptr) : std::make_shared<gio_client>(context,address,port,app::transfer_limits());
        if(!peer->valid()) {app::reply(std::move(done),status::rejected);return;}
        auto completion=std::make_shared<io_completion>(std::move(done));
        const auto receive_started=app::metric_stamp::now(trace_migration_metrics);
        auto pull=std::make_shared<app::migration_pull>(peer,config.credentials[2],record->ticket,record->receiver,
            [this,record,completion,receive_started](status code,const migration::manifest& manifest) mutable noexcept {
                const auto restore_started=app::metric_stamp::now(trace_migration_metrics);
                app::emit_metric("target_receive_and_hash",receive_started,restore_started);
                if(record->metrics) {
                    record->metrics->digest.emit("target_hash");record->metrics->decode.emit("target_reply_decode");
                    record->metrics->append.emit("target_copy_and_hash");
                }
                CAPSTAN_TRY {
                    if(record->phase==import_phase::aborted) code=status::closed;
                    if(code==status::ok) {
                        code=record->receiver->seal(manifest,[&](const migration::buffer& bytes,const std::array<std::uint8_t,32>& hash) noexcept {
                            CAPSTAN_TRY {
                                (void)hash; // Receiver verified the ordered incremental SHA-256 before Seal.
                                if(!record->receiver->digest_valid()) return status::invalid;
                                migration::package_parts parts;
                                auto result=migration::decode_package_parts(bytes,parts);if(result!=status::ok) return result;
                                migration::authority_image history;
                                result=migration::decode_authority_image(parts.history.data,parts.history.size,history);
                                if(result!=status::ok) return result;
                                std::unique_ptr<object_type> binding;
                                result=backend->restore(config.key,parts.snapshot,binding);
                                if(result!=status::ok) return result;
                                if(!binding || binding->resource_bytes()>backend->restore_reservation(record->limit)) return status::exhausted;
                                result=authority.stage_import(record->ticket,history,record->successor);
                                if(result==status::ok) record->restored=std::move(binding);
                                return result;
                            } CAPSTAN_CATCH_ALL {return status::exhausted;}
                        });
                        if(code==status::ok) {record->manifest=manifest;record->phase=import_phase::prepared;}
                    }
                } CAPSTAN_CATCH_ALL {code=status::exhausted;}
                app::emit_metric("target_restore",restore_started,app::metric_stamp::now(trace_migration_metrics));
                record->pulling=false;
                app::reply(std::exchange(*completion,nullptr),code);
            },record->metrics);
        record->pulling=true;record->phase=import_phase::receiving;pull->start();
    }
    void handle_import(const wire::codec::request& request,io_completion done) {
        const auto& args=request.args;migration::ticket t;
        if(args.empty() || !wire::is_string(args[0]) || !migration::decode_ticket(wire::string(args[0]),t)) {app::reply(std::move(done),status::invalid);return;}
        if(t.control!=config.control) {app::reply(std::move(done),status::wrong_control);return;}
        if(t.target_host!=host_id || t.target_instance!=host_instance || t.source.key!=config.key) {app::reply(std::move(done),status::wrong_object);return;}
        if(request.method==migration::reserve_command) {
            grant successor;
            if((args.size()!=3 && args.size()!=5) || !wire::is_string(args[1]) || !wire::try_decode_grant(wire::string(args[1]),successor) || !wire::is_number(args[2])) {app::reply(std::move(done),status::invalid);return;}
            if(args.size()==5 && (!wire::is_string(args[3]) || !wire::is_number(args[4]) || !wire::number(args[4]) || wire::number(args[4])>65535)) {app::reply(std::move(done),status::invalid);return;}
            auto code=reserve_destination(t,successor,wire::number(args[2]));
            if(code==status::ok && args.size()==5) {
                auto record=imports.at(t.sequence);auto address=wire::string(args[3]);auto port=static_cast<unsigned short>(wire::number(args[4]));
                if(record->peer_port && (record->peer_address!=address || record->peer_port!=port)) code=status::conflict;
                else if(record->phase==import_phase::reserved && !record->prepared_peer) {
                    auto peer=std::make_shared<gio_client>(context,address,port,app::transfer_limits());
                    if(!peer->valid()) code=status::rejected;
                    else {record->peer_address=std::move(address);record->peer_port=port;record->prepared_peer=std::move(peer);}
                }
            }
            app::reply(std::move(done),code);return;
        }
        if(args.size()!=(request.method==migration::pull_command ? 3u : 1u)) {app::reply(std::move(done),status::invalid);return;}
        auto found=imports.find(t.sequence);
        if(found!=imports.end() && !(found->second->ticket==t)) {app::reply(std::move(done),status::conflict);return;}
        if(request.method==migration::import_abort_command) {
            auto code=authority.abort_import(t);
            if(code==status::ok && found!=imports.end()) {
                auto& record=*found->second;record.phase=import_phase::aborted;
                record.receiver->abort(record.receiver->identity());record.restored.reset();record.prepared_peer.reset();
                import_reserved-=record.reserved;record.reserved=0;
            }
            app::reply(std::move(done),code);return;
        }
        if(found==imports.end()) {app::reply(std::move(done),status::not_seen);return;}
        auto record=found->second;
        if(request.method==migration::pull_command) {
            if(!wire::is_string(args[1]) || !wire::is_number(args[2]) || !wire::number(args[2]) || wire::number(args[2])>65535) {app::reply(std::move(done),status::invalid);return;}
            pull_destination(record,wire::string(args[1]),static_cast<unsigned short>(wire::number(args[2])),std::move(done));return;
        }
        if(request.method==migration::import_query_command) {
            app::reply(std::move(done),status::ok,{wire::u(static_cast<unsigned>(record->phase))});return;
        }
        if(record->phase==import_phase::active) {
            const auto current=authority.inspect();
            const bool usable=authority.placement_generation()==t.generation+1 && current.gate==host_authority::phase::open && current.grants==std::vector<grant>{record->successor};
            app::reply(std::move(done),usable ? status::ok : status::superseded);return;
        }
        if(record->phase!=import_phase::prepared || !record->restored) {app::reply(std::move(done),status::closed);return;}
        // All data/history is staged before activation. The owner loop publishes
        // the native object before it can process another admitted RPC.
        auto code=authority.activate_import(t);
        if(code==status::ok) {
            owner=std::move(record->restored);record->phase=import_phase::active;
            record->receiver->release_prepared_storage();record->receiver.reset();import_reserved-=record->reserved;record->reserved=0;
        }
        app::reply(std::move(done),code);
    }
    void handle_copy(id holder,const wire::codec::request& r,io_completion done) {
        if(holder!=1 && holder!=2 && holder!=3) {app::reply(std::move(done),status::forbidden);return;}
        if(r.method==migration::copy_query_command) {
            if(holder==3 || r.args.size()!=1 || !wire::is_number(r.args[0]) || !wire::number(r.args[0])) {app::reply(std::move(done),status::invalid);return;}
            auto found=copies.find({holder,wire::number(r.args[0])});
            if(found==copies.end()) {app::reply(std::move(done),status::not_seen);return;}
            const auto& record=*found->second;const auto code=record.released ? status::closed : record.code;
            app::reply(std::move(done),code,code==status::ok ? std::vector<wire::value>{wire::value::string(migration::encode_snapshot_ref(record.reference))} : std::vector<wire::value>{});return;
        }
        if(r.method==migration::copy_command) {
            grant source;
            if(r.args.size()!=2 || !wire::is_string(r.args[0]) || !wire::try_decode_grant(wire::string(r.args[0]),source) ||
               !wire::is_number(r.args[1]) || !wire::number(r.args[1]) || source.holder!=holder) {app::reply(std::move(done),status::invalid);return;}
            const auto sequence=wire::number(r.args[1]);const auto key=std::make_pair(holder,sequence);
            auto old=copies.find(key);
            if(old!=copies.end()) {
                auto& record=*old->second;
                auto code=!(record.source==source) ? status::conflict : record.released ? status::closed : record.code;
                app::reply(std::move(done),code,code==status::ok ? std::vector<wire::value>{wire::value::string(migration::encode_snapshot_ref(record.reference))} : std::vector<wire::value>{});return;
            }
            if(!owner) {app::reply(std::move(done),status::dead);return;}
            const auto native=owner->resource_bytes(),snapshot=owner->snapshot_bytes();
            if(!snapshot || snapshot>export_budget/2) {app::reply(std::move(done),status::exhausted);return;}
            const auto reserve=2*snapshot;
            if(copies.size()>=64 || native>export_budget || reserve>export_budget-native ||
               export_reserved+import_reserved+other_bytes()>export_budget-native-reserve) {app::reply(std::move(done),status::exhausted);return;}
            auto record=std::make_shared<copy_record>();record->source=source;record->reserved=reserve;
            record->reference.source=source.key;record->reference.host=host_id;record->reference.instance=host_instance;
            record->reference.holder=holder;record->reference.sequence=sequence;
            record->payload=std::make_shared<migration::buffer>();
            auto operation=std::make_shared<pending>();operation->call.header={source,sequence,method::summary};operation->copy=record;
            copies.emplace(key,record);copy_reserved+=reserve;
            host_authority::admission admission{status::invalid};
            CAPSTAN_TRY {
                admission=authority.admit(operation->call.header,holder);
                if(admission.code==status::ok) operations.emplace(admission.pin,operation);
            } CAPSTAN_CATCH_ALL {
                if(admission.code==status::ok) authority.dispatch_failed(admission.pin);
                copies.erase(key);copy_reserved-=reserve;CAPSTAN_RETHROW;
            }
            if(admission.code!=status::ok) {copies.erase(key);copy_reserved-=reserve;app::reply(std::move(done),admission.code);return;}
            operation->done=std::move(done);++admitted;kick();return;
        }
        migration::snapshot_ref ref;
        if(r.args.size()!=(r.method==migration::copy_chunk_command ? 2u : 1u) || !wire::is_string(r.args[0]) ||
           !migration::decode_snapshot_ref(wire::string(r.args[0]),ref)) {app::reply(std::move(done),status::invalid);return;}
        if(ref.host!=host_id || ref.instance!=host_instance || ref.source!=config.key) {app::reply(std::move(done),status::wrong_object);return;}
        if(holder!=3 && holder!=ref.holder) {app::reply(std::move(done),status::forbidden);return;}
        auto found=copies.find({ref.holder,ref.sequence});
        if(found==copies.end()) {app::reply(std::move(done),status::not_seen);return;}
        auto& record=*found->second;
        if(record.code==status::pending) {app::reply(std::move(done),status::pending);return;}
        if(!(record.reference==ref)) {app::reply(std::move(done),status::conflict);return;}
        if(r.method==migration::copy_release_command) {
            if(record.code==status::pending) {app::reply(std::move(done),status::busy);return;}
            record.released=true;record.payload.reset();copy_reserved-=record.reserved;record.reserved=0;
            app::reply(std::move(done),status::ok);return;
        }
        if(record.released || record.code!=status::ok) {app::reply(std::move(done),status::closed);return;}
        if(!wire::is_number(r.args[1])) {app::reply(std::move(done),status::invalid);return;}
        auto offset=wire::number(r.args[1]);
        if(offset>=ref.size || offset%migration::transfer_receiver::chunk_bytes) {app::reply(std::move(done),status::invalid);return;}
        auto size=std::min<std::uint64_t>(std::uint64_t(migration::transfer_receiver::chunk_bytes),ref.size-offset);
        auto first=record.payload->begin()+static_cast<std::size_t>(offset);
        app::reply(std::move(done),status::ok,{wire::u(offset),wire::value::string(std::string(first,first+static_cast<std::size_t>(size)))});
    }
    void handle(id,bytes frame,io_completion done) {
        CAPSTAN_TRY {
            app::authenticated auth;wire::codec::request decoded;
            if(!app::try_authenticate(config,frame,auth) || !wire::try_decode(auth.payload,decoded)) {app::reply(std::move(done),status::invalid);return;}
            handle_authenticated(std::move(auth),std::move(decoded),std::move(done));
        } CAPSTAN_CATCH_ALL {if(done) app::reply(std::move(done),status::invalid);}
    }
    // Registry has already authenticated/decoded this frame. Avoid a second
    // envelope round trip on the unchanged scalar RPC hot path.
    void handle_authenticated(app::authenticated auth,wire::codec::request decoded,io_completion done) {
        CAPSTAN_TRY {
            if(!hooks::healthy(*backend)) {done({outcome::unknown,{}});return;}
            if(!hooks::allows(decoded.method)) {app::reply(std::move(done),status::forbidden);return;}
            if(decoded.method==migration::route_command) {
                const auto& a=decoded.args;
                if(a.size()!=4 || !wire::is_number(a[0]) || !wire::is_number(a[1]) || !wire::is_number(a[2]) || !wire::is_string(a[3]) ||
                   object_key{wire::number(a[0]),wire::number(a[1]),wire::number(a[2])}!=config.key) {app::reply(std::move(done),status::wrong_object);return;}
                auth.payload=wire::binary(wire::string(a[3]));
                if(!wire::try_decode(auth.payload,decoded)) {app::reply(std::move(done),status::invalid);return;}
            }
            if(decoded.method>=migration::copy_command && decoded.method<=migration::copy_query_command) {
                auto slot=std::make_shared<io_completion>();*slot=std::move(done);
                CAPSTAN_TRY {handle_copy(auth.holder,decoded,[slot](io_result io) {auto done=std::exchange(*slot,nullptr);if(done) done(std::move(io));});}
                CAPSTAN_CATCH_ALL {app::reply(std::exchange(*slot,nullptr),status::exhausted);}
                return;
            }
            if(decoded.method>=migration::reserve_command && decoded.method<=migration::activate_command) {
                if(auth.holder!=3) {app::reply(std::move(done),status::wrong_control);return;}
                ++control_calls;
                auto slot=std::make_shared<io_completion>();*slot=std::move(done);
                CAPSTAN_TRY {handle_import(decoded,[slot](io_result r) {auto done=std::exchange(*slot,nullptr);if(done) done(std::move(r));});}
                CAPSTAN_CATCH_ALL {app::reply(std::exchange(*slot,nullptr),status::exhausted);}
                return;
            }
            if((decoded.method>=migration::freeze_command && decoded.method<=migration::reserve_export_command) || decoded.method==migration::retire_command) {
                if(auth.holder!=3) {app::reply(std::move(done),status::wrong_control);return;}
                ++control_calls;
                if(decoded.method==migration::host_info_command) {
                    if(!decoded.args.empty()) {app::reply(std::move(done),status::invalid);return;}
                    app::reply(std::move(done),status::ok,{wire::u(host_id),wire::u(host_instance),wire::u(authority.placement_generation()),wire::u(export_limit())});return;
                }
                const bool abort=decoded.method==migration::abort_command;
                const bool chunk=decoded.method==migration::export_chunk_command;
                const bool reserve=decoded.method==migration::reserve_export_command;
                migration::ticket ticket;
                if(decoded.args.size()!=((abort || chunk || reserve)?2u:1u) || !wire::is_string(decoded.args[0]) ||
                   !migration::decode_ticket(wire::string(decoded.args[0]),ticket)) {app::reply(std::move(done),status::invalid);return;}
                if(ticket.source_host!=host_id || ticket.source_instance!=host_instance) {app::reply(std::move(done),status::wrong_object);return;}
                if(ticket.control!=config.control) {app::reply(std::move(done),status::wrong_control);return;}
                if(reserve) {
                    if(!wire::is_number(decoded.args[1]) || wire::number(decoded.args[1])>export_budget) {app::reply(std::move(done),status::invalid);return;}
                    auto code=reserve_source(ticket,static_cast<std::size_t>(wire::number(decoded.args[1])));app::reply(std::move(done),code);return;
                }
                if(decoded.method==migration::retire_command) {
                    migration::authority_phase phase;auto code=authority.migration_status(ticket,phase);
                    if(code==status::ok && (phase!=migration::authority_phase::fenced || authority.placement_generation()!=ticket.generation)) code=status::conflict;
                    if(code==status::ok && owner) {owner.reset();++destroyed;}
                    app::reply(std::move(done),code);return;
                }
                if(decoded.method==migration::export_release_command) {
                    migration::authority_phase phase;
                    auto code=authority.migration_status(ticket,phase);
                    if(code!=status::ok) {app::reply(std::move(done),code);return;}
                    if(phase!=migration::authority_phase::fenced && phase!=migration::authority_phase::aborted && phase!=migration::authority_phase::installed) {app::reply(std::move(done),status::busy);return;}
                    auto found=exports.find(ticket.sequence);
                    if(found!=exports.end()) {
                        if(!(found->second->ticket==ticket)) {app::reply(std::move(done),status::conflict);return;}
                        if(found->second->started && found->second->code==status::pending) {app::reply(std::move(done),status::busy);return;}
                        export_reserved-=found->second->reserved;
                        exports.erase(found);
                    }
                    app::reply(std::move(done),status::ok);return;
                }
                if(decoded.method==migration::export_command) {export_snapshot(ticket,std::move(done));return;}
                if(chunk) {
                    if(!wire::is_number(decoded.args[1])) {app::reply(std::move(done),status::invalid);return;}
                    auto found=exports.find(ticket.sequence);
                    if(found==exports.end()) {app::reply(std::move(done),status::not_seen);return;}
                    if(!(found->second->ticket==ticket)) {app::reply(std::move(done),status::conflict);return;}
                    auto& record=*found->second;
                    if(record.code!=status::ok) {app::reply(std::move(done),record.code);return;}
                    auto offset=wire::number(decoded.args[1]);
                    if(offset>=record.payload->size() || offset%migration::transfer_receiver::chunk_bytes) {app::reply(std::move(done),status::invalid);return;}
                    auto length=std::min<std::uint64_t>(std::uint64_t(migration::transfer_receiver::chunk_bytes),record.payload->size()-offset);
                    auto first=record.payload->begin()+static_cast<std::size_t>(offset);
                    app::reply(std::move(done),status::ok,{wire::u(offset),wire::value::string(std::string(first,first+static_cast<std::size_t>(length)))});return;
                }
                if(decoded.method==migration::freeze_command) {
                    if(trace_migration_metrics && metric_sequence!=ticket.sequence) {
                        metric_sequence=ticket.sequence;business_drain_reported=barrier_reported=false;
                        const auto occupancy=authority.inspect_occupancy();
                        std::cout<<"OCCUPANCY readers="<<occupancy.readers<<" writers="<<occupancy.writers<<" captures="<<occupancy.captures<<'\n'<<std::flush;
                        freeze_metric=app::metric_stamp::now();
                    }
                    migration_replies.freeze(ticket,std::move(done));prefetch_read_snapshot(ticket);collect_resource();return;
                }
                status code;
                if(abort) {
                    grant replacement;
                    if(!wire::is_string(decoded.args[1]) || !wire::try_decode_grant(wire::string(decoded.args[1]),replacement)) {app::reply(std::move(done),status::invalid);return;}
                    migration_replies.abort(ticket,replacement,std::move(done));collect_resource();return;
                } else if(decoded.method==migration::fence_command) {
                    code=authority.fence_migration(ticket);
                    if(code==status::ok && export_ready_metric.wall) {
                        app::emit_metric("source_serve_until_fence",export_ready_metric,app::metric_stamp::now());export_ready_metric={};
                    }
                }
                else {
                    migration::authority_phase phase;code=authority.migration_status(ticket,phase);
                    app::reply(std::move(done),code,code==status::ok ? std::vector<wire::value>{wire::value::string(migration::encode_ticket(ticket)),wire::u(static_cast<unsigned>(phase))} : std::vector<wire::value>{});return;
                }
                app::reply(std::move(done),code);return;
            }
            if(decoded.method==wire::info) {
                app::reply(std::move(done),status::ok,{wire::u(config.key.cluster),wire::u(config.key.incarnation),wire::u(config.key.object),wire::u(owner ? owner->resource_bytes():0)}); return;
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
                    // An empty placement cannot be bootstrapped into serving
                    // authority without a restored native object.
                    if(!owner && authority.inspect().epoch==0) {app::reply(std::move(done),status::closed);return;}
                    code=control_replies.set(t,std::move(done));
                    std::cout<<"TRANSITION tx="<<t.tx<<" epoch="<<authority.inspect().epoch<<" status="<<status_name(code)<<" active="<<authority.inspect().active<<'\n'<<std::flush;
                    collect_resource(); return;
                } else {if(!wire::is_number(decoded.args[0])) {app::reply(std::move(done),status::invalid);return;} tx=wire::number(decoded.args[0]);code=status::ok;}
                if(!collect_resource()) {done({outcome::unknown,{}});return;}
                acknowledgement ack;
                if(code==status::ok) code=authority.ack(tx,ack);
                app::reply(std::move(done),code,code==status::ok ? std::vector<wire::value>{wire::value::string(wire::binary(wire::encode_transition(ack.installed)))} : std::vector<wire::value>{}); return;
            }
            if(decoded.method==wire::stats) {
                auto s=authority.inspect();
                app::reply(std::move(done),status::ok,{wire::u(admitted),wire::u(completed),wire::u(rejected),wire::u(control_calls),wire::u(s.active),wire::u(s.epoch),wire::u(destroyed)}); return;
            }
            wire::call call;if(!wire::try_decode_call(auth.payload,call)) {app::reply(std::move(done),status::invalid);return;}
            bytes prior;if(hooks::replay(*backend,call,auth.holder,prior)) {done({outcome::completed,std::move(prior)});return;}
            if(!hooks::healthy(*backend)) {done({outcome::unknown,{}});return;}
            // Store payload before admission so allocation failure cannot leave
            // an accepted operation without its corresponding execution frame.
            auto frame_owner=std::make_shared<pending>();
            frame_owner->call=std::move(call);frame_owner->done=std::move(done);
            host_authority::admission admission{status::invalid};
            CAPSTAN_TRY {admission=authority.admit(frame_owner->call.header,auth.holder);}
            CAPSTAN_CATCH_ALL {app::reply(std::move(frame_owner->done),status::rejected);return;}
            if(admission.code!=status::ok) {
                ++rejected; if(trace_requests) std::cout<<"REJECT status="<<status_name(admission.code)<<'\n'<<std::flush;
                app::reply(std::move(frame_owner->done),admission.code); return;
            }
            CAPSTAN_TRY { operations.emplace(admission.pin,frame_owner); }
            CAPSTAN_CATCH_ALL {authority.dispatch_failed(admission.pin); collect_resource(); app::reply(std::move(frame_owner->done),status::rejected); return;}
            ++admitted; if(trace_requests) std::cout<<"ADMIT pin="<<admission.pin<<" epoch="<<authority.inspect().epoch<<" active="<<authority.inspect().active<<'\n'<<std::flush;
            kick();
        } CAPSTAN_CATCH_ALL { if(done) app::reply(std::move(done),status::invalid); }
    }
};
