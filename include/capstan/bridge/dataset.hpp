#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/protocol/wire.hpp>
#include <capstan/bridge/dataset_value.hpp>
#include "examples/dataset/dataset.hpp"
#include <atomic>
#include <memory>
#include <typeindex>
#define DYNABRIDGE_IMPORT_DEF "contracts/dataset_import.def"
#define DYNABRIDGE_EXPORT_DEF "contracts/dataset_export.def"
#include <dynabridge/bridge.h>
#undef DYNABRIDGE_IMPORT_DEF
#undef DYNABRIDGE_EXPORT_DEF

namespace capstan {
struct dataset_backend : dynabridge::backend_base<dataset_backend> {
    using dynamic_value_t = dynabridge::rpc::value;
    template<class T> struct converter : dynabridge::rpc_backend::converter<T> {};
    struct context {
        using backend_t=dataset_backend;
        object_key key{};
        void* proxy=nullptr;
        std::type_index proxy_type{typeid(void)};
        std::atomic<std::uint64_t> lower_count{0},bind_count{0};
    };
    template<class Receiver,class Direction=typename dynabridge::bridge_direction<Receiver>::type>
    class object_t : public dynabridge::object_base_selector<object_t<Receiver,Direction>,dataset_backend,Receiver,Direction>::type {
        grant grant_;
    public:
        explicit object_t(grant g) noexcept : grant_(std::move(g)) {}
        const grant& capability() const noexcept { return grant_; }
        Receiver* native(context& ctx) const {
            if(ctx.key!=grant_.key || !ctx.proxy || ctx.proxy_type!=std::type_index(typeid(Receiver)))
                CAPSTAN_RAISE(std::runtime_error("object receiver mismatch"));
            return static_cast<Receiver*>(ctx.proxy);
        }
    };
    template<class Class,class Receiver>
    static dynamic_value_t to_dynamic_object_impl(context& ctx,const object_t<Receiver,dynabridge::import_t>& object) {
        ++ctx.lower_count;
        return dynamic_value_t::string(wire::encode_grant(object.capability()));
    }
    template<class Class>
    static object_t<Class,dynabridge::export_t> bind_export_object_impl(context& ctx,const dynamic_value_t& v) {
        if(v.kind()!=dynabridge::rpc::value_kind::string) CAPSTAN_RAISE(dynabridge::bad_conversion("object receiver must be capability reference"));
        object_t<Class,dynabridge::export_t> object(wire::decode_grant(v.copy_string()));
        (void)object.native(ctx);
        ++ctx.bind_count;
        return object;
    }
};
template<> struct dataset_backend::converter<std::uint64_t> {
    template<class C> static dynamic_value_t to(C&,std::uint64_t n) DYNABRIDGE_NOEXCEPT_IF_DISABLED { return dynamic_value_t::unsigned_integer(n); }
    template<class C> static dynabridge::optional<std::uint64_t> from(C&,const dynamic_value_t& v) DYNABRIDGE_NOEXCEPT_IF_DISABLED {
        if(v.kind()!=dynabridge::rpc::value_kind::unsigned_integer) return {};
        return dynabridge::optional<std::uint64_t>(v.as_unsigned());
    }
};
template<> struct dataset_backend::converter<summary_t> {
    template<class C> static dynamic_value_t to(C&,const summary_t& s) DYNABRIDGE_NOEXCEPT_IF_DISABLED {
        auto bytes=dynabridge::rpc::detail::encode_request(901,{dynamic_value_t::unsigned_integer(s.count),dynamic_value_t::unsigned_integer(s.sum),dynamic_value_t::unsigned_integer(s.checksum),dynamic_value_t::unsigned_integer(s.version)});
        return dynamic_value_t::string(std::string(bytes.begin(),bytes.end()));
    }
    template<class C> static dynabridge::optional<summary_t> from(C&,const dynamic_value_t& v) DYNABRIDGE_NOEXCEPT_IF_DISABLED {
        if(v.kind()!=dynabridge::rpc::value_kind::string) return {};
        wire::codec::request msg;
        auto text=v.copy_string();
        if(!wire::codec::try_decode_request(bytes(text.begin(),text.end()),msg)) return {};
        if(msg.method!=901 || msg.args.size()!=4) return {};
        for(auto& arg:msg.args) if(!wire::is_number(arg)) return {};
        return dynabridge::optional<summary_t>(summary_t{msg.args[0].as_unsigned(),msg.args[1].as_unsigned(),msg.args[2].as_unsigned(),msg.args[3].as_unsigned()});
    }
};
using dataset_delegate=dynabridge::dataset<dataset_backend::context>;

// The descriptor uses the generated member's overload list to choose its
// declaration and the existing import_argument + object-channel lowering hooks.
// The synchronous generated member is not called and its return type is not
// rewritten; only the explicit descriptor introduces asynchronous execution.
template<class Result> struct typed_call {
    using result_type=Result;
    request header;
    std::vector<wire::value> args;
    bytes encode(id request_id) const { auto h=header; h.request_id=request_id; return wire::encode_call(h,args); }
    static bool try_decode(const wire::reply& reply,Result& out) {
        if(reply.state!=outcome::completed || reply.code!=status::ok || reply.values.size()!=1) return false;
        dataset_backend::context ctx;
        auto converted=dynabridge::from_optional<Result>(ctx,reply.values[0]);
        if(!converted) return false;
        out=std::move(*converted);return true;
    }
    static Result decode(const wire::reply& reply) {
        Result out{};
        if(!try_decode(reply,out)) CAPSTAN_RAISE(std::runtime_error("call has no completed typed result"));
        return out;
    }
};
template<class Contract> struct lower_dataset_call;
template<class R,class... Declared>
struct lower_dataset_call<dynabridge::free_callable<R(Declared...)>> {
    template<class... Actual>
    static typed_call<R> make(method which,dataset_delegate& receiver,Actual&&... args) {
        auto& ctx=receiver.context();
        auto dynamic=dataset_backend::to_dynamic_object<dynabridge::import_symbols::dataset>(ctx,receiver.object());
        request header{wire::decode_grant(dynamic.copy_string()),0,which};
        return {header,{dynabridge::import_argument<Declared,dataset_backend::context>::lower(ctx,std::forward<Actual>(args))...}};
    }
};
template<class Symbol,class... Args>
auto describe_call(method which,dataset_delegate& receiver,Args&&... args) {
    using selector=dynabridge::import_contract_selector_group<typename Symbol::overloads_t,dataset_backend::context>;
    using contract=typename decltype(selector::select(std::declval<Args>()...))::type;
    return lower_dataset_call<contract>::make(which,receiver,std::forward<Args>(args)...);
}
class dataset_facade {
    mutable dataset_delegate receiver_;
public:
    dataset_facade(dataset_backend::context& ctx,grant g) : receiver_(ctx,dataset_delegate::object_t(g)) {}
    typed_call<std::uint64_t> version() const { return describe_call<dynabridge::import_symbols::dataset::version>(method::version,receiver_); }
    typed_call<summary_t> summary() const { return describe_call<dynabridge::import_symbols::dataset::summary>(method::summary,receiver_); }
    typed_call<std::uint64_t> add_range(std::uint64_t b,std::uint64_t e,std::uint64_t d) {
        return describe_call<dynabridge::import_symbols::dataset::add_range>(method::add_range,receiver_,b,e,d);
    }
};
class dataset_binding {
    // Ordering keeps the native referent before its generated borrowed proxy.
    dynabridge::capstan_native::dataset native_;
    dynabridge::exports::dataset proxy_;
    dataset_backend::context ctx_;
public:
    explicit dataset_binding(object_key key,std::size_t bytes=64u*1024u*1024u,std::uint64_t seed=1)
        : native_(bytes,seed),proxy_(dynabridge::borrow(native_)) { ctx_.key=key; ctx_.proxy=&proxy_; ctx_.proxy_type=typeid(decltype(proxy_)); }
    dataset_binding(const dataset_binding&)=delete;
    dataset_binding& operator=(const dataset_binding&)=delete;
    std::size_t resource_bytes() const noexcept { return native_.resource_bytes(); }
    std::uint64_t bind_count() const noexcept { return ctx_.bind_count.load(); }
    // Caller must hold an admitted Host operation pin and the object lane.
    bool try_dispatch(method which,const grant& g,const std::vector<wire::value>& args,wire::value& out) {
        if(g.key!=ctx_.key) return false;
        auto receiver=wire::value::string(wire::encode_grant(g));
        switch(which) {
        case method::version: {
            if(!args.empty()) return false;
            auto binder=dynabridge::create_export_member_callable_binder<dynabridge::exports::dataset,std::uint64_t()>(ctx_,&dynabridge::capstan_native::dataset::version);
            out=binder(receiver);return true;
        }
        case method::summary: {
            if(!args.empty()) return false;
            auto binder=dynabridge::create_export_member_callable_binder<dynabridge::exports::dataset,summary_t()>(ctx_,&dynabridge::capstan_native::dataset::summary);
            out=binder(receiver);return true;
        }
        case method::add_range: {
            if(args.size()!=3) return false;
            for(const auto& arg:args) if(!wire::is_number(arg)) return false;
            if(!native_.can_add_range(wire::number(args[0]),wire::number(args[1]))) return false;
            auto binder=dynabridge::create_export_member_callable_binder<dynabridge::exports::dataset,std::uint64_t(std::uint64_t,std::uint64_t,std::uint64_t)>(ctx_,&dynabridge::capstan_native::dataset::add_range);
            out=binder(receiver,args[0],args[1],args[2]);return true;
        }
        }
        return false;
    }
    wire::value dispatch(method which,const grant& g,const std::vector<wire::value>& args) {
        wire::value out;
        if(!try_dispatch(which,g,args,out)) CAPSTAN_RAISE(dynabridge::bad_conversion("invalid dataset call"));
        return out;
    }
};
}
