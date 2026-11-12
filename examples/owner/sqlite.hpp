#pragma once
#include "single_process.hpp"
#include <capstan/durable/sqlite_store.hpp>
#include <capstan/durable/images.hpp>
#include <capstan/owner/runtime_hooks.hpp>
#include <atomic>
namespace capstan { namespace examples {
// Local storage: a base snapshot plus ordered deterministic redo records.
// Logical state head + delta + canonical request/result commit in one transaction.
// This first backend supports one object and directory authority transactions.
struct sqlite_owner_backend {
    struct object_type {
        std::unique_ptr<dataset_binding> native;
        const std::size_t size;
        explicit object_type(std::unique_ptr<dataset_binding> p):native(std::move(p)),size(native->resource_bytes()) {}
        std::size_t resource_bytes() const noexcept {return size;}
        std::size_t snapshot_bytes() const noexcept {return size+60;}
        bool try_dispatch(method,const grant&,const std::vector<wire::value>&,wire::value&) PORTAL_NOEXCEPT_IF_DISABLED {return false;} // Durable execution requires request identity.
        status capture_snapshot(migration::buffer& b) const PORTAL_NOEXCEPT_IF_DISABLED {return native->capture_snapshot(b);}
        status append_snapshot(migration::buffer& b) const PORTAL_NOEXCEPT_IF_DISABLED {return native->append_snapshot(b);}
    };
    static constexpr bool concurrent_read_capture=false;
    std::shared_ptr<durable::sqlite_store> store;
    std::atomic<bool> failed{false};
    object_key key{};
    std::string prefix;
    bytes last_authority;
    id base_version=0;
    std::size_t receipt_limit=4096,receipts=0;
    durable::fault_hook fault=nullptr;
    explicit sqlite_owner_backend(const std::string& path):store(std::make_shared<durable::sqlite_store>(path)) {
        if(!store->claim_actor("host")) CAPSTAN_RAISE(std::runtime_error("persistent Host already running"));
    }
    bool can_inline(method) const noexcept {return false;}
    std::size_t restore_reservation(std::size_t n) const noexcept {return n;}
    bool healthy() const noexcept {return !failed.load() && store->healthy();}
    void fail() noexcept {failed.store(true);}
    bool put(const std::vector<durable::record>& batch) noexcept {
        if(!healthy() || store->commit(batch,fault).outcome!=durable::commit_outcome::committed) {fail();return false;}return true;
    }
    std::string receipt_key(id holder,id request) const {return prefix+"receipt/"+std::to_string(holder)+"/"+std::to_string(request);}
    static bytes manifest(id base,id head) {
        durable::image_writer w(0x32445343);w.number(base);w.number(head);return std::move(w.value);
    }
    std::string delta_key(id version) const {
        auto n=std::to_string(version);return prefix+"delta/"+std::string(20-n.size(),'0')+n;
    }
    // A storage envelope replaces host/data: old binaries fail to decode it
    // instead of silently serving the base snapshot while ignoring redo.
    bool restore_incremental(const bytes& image,migration::dataset_snapshot& data) {
        durable::image_reader m(image,0x32445343);id head;
        if(!m.number(base_version)||!m.number(head)||!m.end()||head<base_version||head-base_version>1048576) return false;
        bytes base;bool found;
        if(!store->get(prefix+"base",base,found)||!found||migration::decode(base,data)!=status::ok||data.version!=base_version) return false;
        std::vector<durable::record> deltas;
        if(!store->list(prefix+"delta/",deltas,1048576)||deltas.size()!=head-base_version) return false;
        for(const auto& row:deltas) {
            durable::image_reader d(row.value,0x314c4443);id before,begin,end,delta,holder,request;
            if(!d.number(before)||!d.number(begin)||!d.number(end)||!d.number(delta)||!d.number(holder)||!d.number(request)||!d.end()||
               row.key!=delta_key(before+1)||!request) return false;
            // Validate atomic coupling to its successful, canonical receipt.
            bytes rb,canonical,response;
            if(!store->get(receipt_key(holder,request),rb,found)||!found) return false;
            durable::image_reader r(rb,0x31524343);
            if(!r.blob(canonical,wire::max_frame)||!r.blob(response,wire::max_frame)||!r.end()) return false;
            wire::call call;wire::reply reply;
            if(!wire::try_decode_call(canonical,call)||!wire::try_decode_reply(response,reply)||call.header.operation!=method::add_range||
               call.header.capability.key!=key||call.header.capability.holder!=holder||call.header.request_id!=request||call.args.size()!=3||
               reply.state!=outcome::completed||reply.code!=status::ok||reply.values.size()!=1||!wire::is_number(reply.values[0])||wire::number(reply.values[0])!=before+1) return false;
            for(const auto& v:call.args) if(!wire::is_number(v)) return false;
            if(wire::number(call.args[0])!=begin||wire::number(call.args[1])!=end||wire::number(call.args[2])!=delta) return false;
            if(!portal::capstan_native::dataset::replay_add(data,{before,begin,end,delta})) return false;
        }
        return data.version==head;
    }
    std::unique_ptr<object_type> create(object_key k,std::size_t size,id seed) PORTAL_NOEXCEPT_IF_DISABLED {
        if(!prefix.empty()) return nullptr;
        key=k;prefix="host/";bytes snapshot;bool found;
        if(!store->get(prefix+"data",snapshot,found)) return nullptr;
        std::unique_ptr<dataset_binding> native;
        single_process_owner_backend memory;
        if(found) {
            bytes identity;
            if(!store->get(prefix+"identity",identity,found)||!found) {fail();return nullptr;}
            durable::image_reader r(identity,0x31444943);object_key saved;
            if(!r.number(saved.cluster)||!r.number(saved.incarnation)||!r.number(saved.object)||!r.end()||saved!=k) {fail();return nullptr;}
            migration::dataset_snapshot data;
            if(migration::decode(snapshot,data)==status::ok) {
                // Atomic legacy upgrade; never rewrite identity or receipts.
                std::vector<durable::record> orphan;
                bytes oldbase;bool hasbase;
                if(!store->get(prefix+"base",oldbase,hasbase)||hasbase||!store->list(prefix+"delta/",orphan,1)||!orphan.empty()) {fail();return nullptr;}
                base_version=data.version;
                if(!put({{prefix+"base",snapshot},{prefix+"data",manifest(base_version,base_version)}})) return nullptr;
            } else if(!restore_incremental(snapshot,data)) {fail();return nullptr;}
            auto restored=portal::capstan_native::dataset::restore(std::move(data));
            if(!restored) {fail();return nullptr;}
            native.reset(new dataset_binding(k,std::move(*restored)));
        } else {
            std::vector<durable::record> existing;
            if(!store->list(prefix,existing) || !existing.empty()) {fail();return nullptr;}
            native=memory.create(k,size,seed);
            if(native->capture_snapshot(snapshot)!=status::ok) return nullptr;
            durable::image_writer identity(0x31444943);identity.number(k.cluster);identity.number(k.incarnation);identity.number(k.object);
            if(!put({{prefix+"identity",std::move(identity.value)},{prefix+"base",std::move(snapshot)},{prefix+"data",manifest(0,0)}})) return nullptr;
        }
        return std::unique_ptr<object_type>(new object_type(std::move(native)));
    }
    status restore(object_key,owner_snapshot_view,std::unique_ptr<object_type>&) PORTAL_NOEXCEPT_IF_DISABLED {return status::forbidden;}
    bool initialize(host_authority& authority,id control,std::size_t limit) {
        if(prefix.empty()) {fail();return false;}
        receipt_limit=limit;bytes b;bool found;
        if(!store->get(prefix+"authority",b,found)) return false;
        if(found) {host_authority::durable_image image;
            if(!durable::decode_host(b,image,limit)||image.key!=key||image.control!=control||!authority.recover(image)) {fail();return false;}
            last_authority=b;
        }
        std::vector<durable::record> history;
        if(!store->list(prefix+"receipt/",history,limit)) return false;
        receipts=history.size();
        // Resume a persisted Close only after committed Dataset restoration.
        authority.finish_recovery();return checkpoint(authority);
    }
    bool checkpoint(host_authority& authority) {
        CAPSTAN_TRY {
            host_authority::durable_image image;
            if(!healthy() || !authority.checkpoint(image)) {fail();return false;}
            auto b=durable::encode_host(image);if(b==last_authority) return true;
            if(!put({{prefix+"authority",b}})) return false;
            last_authority=std::move(b);return true;
        } CAPSTAN_CATCH_ALL {fail();return false;}
    }
    bool replay(const wire::call& call,id holder,bytes& response) {
        if(holder!=call.header.capability.holder || call.header.capability.key!=key || !call.header.request_id) return false;
        bytes b;bool found;
        if(!store->get(receipt_key(holder,call.header.request_id),b,found)) {fail();response=wire::encode_reply(outcome::unknown,status::pending);return true;}
        if(!found) return false;
        durable::image_reader r(b,0x31524343);bytes request,result;
        if(!r.blob(request,wire::max_frame)||!r.blob(result,wire::max_frame)||!r.end()) {fail();response=wire::encode_reply(outcome::unknown,status::invalid);return true;}
        auto canonical=wire::encode_call(call.header,call.args);
        response=canonical==request ? std::move(result) : wire::encode_reply(outcome::rejected,status::conflict);return true;
    }
    bytes execute(object_type& object,const wire::call& call) {
        if(!healthy()) return wire::encode_reply(outcome::unknown,status::pending);
        bytes prior;if(replay(call,call.header.capability.holder,prior)) return prior;
        if(receipts>=receipt_limit) return wire::encode_reply(outcome::rejected,status::exhausted);
        // All allocation/conversion happens before COMMIT. The object lane
        // prevents any other write until the nonthrowing publication completes.
        effect e;if(!method_effect(call.header.operation,e)) return wire::encode_reply(outcome::rejected,status::unknown_method);
        dataset_binding::prepared_add prepared;
        wire::value value;
        const bool writing=e==effect::write;
        const bool ok=writing ? (call.header.operation==method::add_range && object.native->prepare_add(call.header.capability,call.args,prepared)) :
            object.native->try_dispatch(call.header.operation,call.header.capability,call.args,value);
        if(writing && ok) value=wire::u(prepared.before+1);
        auto response=wire::encode_reply(outcome::completed,ok ? status::ok : status::invalid,ok ? std::vector<wire::value>{value} : std::vector<wire::value>{});
        durable::image_writer receipt(0x31524343);receipt.blob(wire::encode_call(call.header,call.args));receipt.blob(response);
        std::vector<durable::record> batch;batch.push_back({receipt_key(call.header.capability.holder,call.header.request_id),std::move(receipt.value)});
        if(writing && ok) {
            durable::image_writer d(0x314c4443);d.number(prepared.before);d.number(prepared.begin);d.number(prepared.end);d.number(prepared.delta);
            d.number(call.header.capability.holder);d.number(call.header.request_id);
            batch.push_back({delta_key(prepared.before+1),std::move(d.value)});
            batch.push_back({prefix+"data",manifest(base_version,prepared.before+1)});
        }
        if(!put(batch)) return wire::encode_reply(outcome::unknown,status::pending);
        if(writing && ok) object.native->publish_add(prepared);
        ++receipts;return response;
    }
};
}}
namespace capstan {
template<> struct owner_runtime_hooks<examples::sqlite_owner_backend> {
    using B=examples::sqlite_owner_backend;
    static constexpr bool durable=true;
    static bool initialize(B& b,host_authority& a,id control,std::size_t limit) PORTAL_NOEXCEPT_IF_DISABLED {return b.initialize(a,control,limit);}
    static bool checkpoint(B& b,host_authority& a) PORTAL_NOEXCEPT_IF_DISABLED {return b.checkpoint(a);}
    static bool healthy(const B& b) noexcept {return b.healthy();}
    static bool allows(id m) noexcept {
        return m==wire::info || m==wire::stats || m==wire::set_authority || m==wire::query_ack ||
               m==wire::query_current_authority || m==wire::fetch_my_grant || (m>=1 && m<=3);
    }
    static bool replay(B& b,const wire::call& c,id holder,bytes& out) PORTAL_NOEXCEPT_IF_DISABLED {return b.replay(c,holder,out);}
    static bytes execute(B& b,B::object_type& o,const wire::call& c) PORTAL_NOEXCEPT_IF_DISABLED {return b.execute(o,c);}
};
}
