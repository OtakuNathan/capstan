#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/protocol/wire.hpp>
#include <capstan/transport/gio.hpp>
#include <capstan/transport/context_dispatch.hpp>
#include <capstan/async/io.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <utility>
#include <cerrno>
#include <cstdlib>
namespace app {
using namespace capstan;
[[noreturn]] inline void startup_failure(const char* message) {std::cerr<<message<<'\n';std::exit(EXIT_FAILURE);}
inline const std::string& required(const std::map<std::string,std::string>& args,const std::string& key) {
    auto it=args.find(key);if(it==args.end()) startup_failure("missing required option");return it->second;
}
struct config {
    object_key key;
    id control;
    std::array<token,3> credentials;
};
inline token parse_token(const std::string& s) {
    if(s.size()!=32) startup_failure("credential must be 16 bytes");
    token t{};
    for(unsigned i=0;i<16;++i) {
        auto hex=[](char c)->unsigned { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; startup_failure("invalid credential"); };
        t[i]=static_cast<unsigned char>(hex(s[2*i])*16+hex(s[2*i+1]));
    }
    return t;
}
inline config load_config(const std::string& path) {
    std::ifstream f(path); config c{};
    f>>c.key.cluster>>c.key.incarnation>>c.key.object>>c.control;
    for(auto& t:c.credentials) { std::string s; f>>s; t=parse_token(s); }
    if(!f || !c.control) startup_failure("invalid lab config");
    return c;
}
inline token load_credential(const std::string& path) { std::ifstream f(path); std::string s; f>>s; return parse_token(s); }
inline std::string token_bytes(const token& t) { return {t.begin(),t.end()}; }
struct authenticated { id holder; bytes payload; };
inline bool try_authenticate(const config& c,const bytes& packet,authenticated& out) {
    wire::codec::request r;
    if(!wire::try_decode(packet,r) || r.method!=9999 || r.args.size()!=2 || !wire::is_string(r.args[0]) || !wire::is_string(r.args[1])) return false;
    auto credential=wire::string(r.args[0]);
    for(unsigned i=0;i<3;++i) if(credential==token_bytes(c.credentials[i])) {
        out={i+1,wire::binary(wire::string(r.args[1]))};return true;
    }
    return false;
}
inline authenticated authenticate(const config& c,const bytes& packet) {
    authenticated out;if(!try_authenticate(c,packet,out)) CAPSTAN_RAISE(std::runtime_error("unknown lab session"));return out;
}
class authenticated_endpoint : public io_endpoint {
    io_endpoint& endpoint_; token credential_;
public:
    authenticated_endpoint(io_endpoint& ep,token t):endpoint_(ep),credential_(t){}
    bool submit(bytes b,io_completion done) noexcept override {
        CAPSTAN_TRY { return endpoint_.submit(wire::command(9999,{wire::value::string(token_bytes(credential_)),wire::value::string(wire::binary(b))}),std::move(done)); }
        CAPSTAN_CATCH_ALL {return false;}
    }
};
inline void reply(io_completion done,status code,std::vector<wire::value> values={}) noexcept {
    io_result result{outcome::unknown,{}};
    CAPSTAN_TRY {result={outcome::completed,wire::encode_reply(code==status::ok ? outcome::completed : outcome::rejected,code,values)};}
    CAPSTAN_CATCH_ALL {}
    // Keep the callback until encoding finishes; a failed encoding must still
    // retire the accepted completion rather than leave the connection pending.
    if(done) {CAPSTAN_TRY {done(std::move(result));} CAPSTAN_CATCH_ALL {}}
}
inline void post(GMainContext* context,std::function<void()> work) {
    post_context(context,std::move(work));
}
inline std::map<std::string,std::string> options(int argc,char** argv) {
    std::map<std::string,std::string> m;
    for(int i=1;i<argc;i+=2) { if(i+1==argc) startup_failure("option requires value"); m.emplace(argv[i],argv[i+1]); }
    return m;
}
inline std::uint64_t option_number(const std::map<std::string,std::string>& a,const std::string& k,id fallback) {
    auto it=a.find(k);if(it==a.end()) return fallback;
    const auto& text=it->second;if(text.empty() || text[0]=='-') startup_failure("invalid numeric option");
    char* end=nullptr;errno=0;auto value=std::strtoull(text.c_str(),&end,10);
    if(errno==ERANGE || end!=text.c_str()+text.size()) startup_failure("invalid numeric option");
    return value;
}
inline unsigned short port_option(const std::map<std::string,std::string>& a,const std::string& k,id fallback) {
    auto v=option_number(a,k,fallback); if(v>65535) startup_failure("invalid port"); return static_cast<unsigned short>(v);
}
}
