#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace capstan {
using id = std::uint64_t;
struct object_key {
    id cluster=0, incarnation=0, object=0;
    bool operator==(const object_key& b) const { return cluster==b.cluster && incarnation==b.incarnation && object==b.object; }
    bool operator!=(const object_key& b) const { return !(*this==b); }
};
using token = std::array<unsigned char,16>;
enum class access_mode : unsigned { shared=1, exclusive=2 };
enum class effect { read, write };
enum class method : unsigned { version=1, summary=2, add_range=3 };
inline bool method_effect(method m, effect& e) {
    switch(m) { case method::version: case method::summary: e=effect::read; return true;
        case method::add_range: e=effect::write; return true; }
    return false;
}
struct grant {
    object_key key;
    id epoch=0, grant_id=0, holder=0;
    access_mode mode=access_mode::shared;
    token secret{};
    bool operator==(const grant& b) const {
        return key==b.key && epoch==b.epoch && grant_id==b.grant_id && holder==b.holder && mode==b.mode && secret==b.secret;
    }
};
struct transition {
    object_key key;
    id control_session=0, tx=0, expected_epoch=0, next_epoch=0;
    bool destroy=false;
    std::vector<grant> grants;
    bool operator==(const transition& b) const {
        return key==b.key && control_session==b.control_session && tx==b.tx && expected_epoch==b.expected_epoch && next_epoch==b.next_epoch && destroy==b.destroy && grants==b.grants;
    }
};
enum class status : unsigned {
    ok, pending, rejected, closed, dead, stale, wrong_object, wrong_control,
    bad_grant, forbidden, unknown_method, duplicate, busy, conflict, exhausted, invalid
};
inline const char* status_name(status s) {
    switch(s) {
#define CAPSTAN_STATUS(x) case status::x: return #x;
    CAPSTAN_STATUS(ok) CAPSTAN_STATUS(pending) CAPSTAN_STATUS(rejected) CAPSTAN_STATUS(closed)
    CAPSTAN_STATUS(dead) CAPSTAN_STATUS(stale) CAPSTAN_STATUS(wrong_object) CAPSTAN_STATUS(wrong_control)
    CAPSTAN_STATUS(bad_grant) CAPSTAN_STATUS(forbidden) CAPSTAN_STATUS(unknown_method) CAPSTAN_STATUS(duplicate)
    CAPSTAN_STATUS(busy) CAPSTAN_STATUS(conflict) CAPSTAN_STATUS(exhausted) CAPSTAN_STATUS(invalid)
#undef CAPSTAN_STATUS
    }
    return "invalid";
}
// Shared by Directory preflight and Host installation. This only checks the
// proposed target; the Host still validates the control session/current epoch.
inline status validate_target(const object_key& key, id epoch, bool destroy, const std::vector<grant>& grants) {
    if (destroy && !grants.empty()) return status::invalid;
    if (grants.size()>64) return status::exhausted;
    for (std::size_t i=0;i<grants.size();++i) {
        const auto& g=grants[i];
        if (g.key!=key || g.epoch!=epoch || !g.grant_id || !g.holder || g.secret==token{}) return status::invalid;
        if (g.mode!=access_mode::shared && g.mode!=access_mode::exclusive) return status::invalid;
        if (g.mode==access_mode::exclusive && grants.size()!=1) return status::invalid;
        for (std::size_t j=0;j<i;++j)
            if (grants[j].grant_id==g.grant_id || grants[j].holder==g.holder || grants[j].secret==g.secret) return status::invalid;
    }
    return status::ok;
}
enum class outcome : unsigned { not_submitted, rejected, completed, unknown };
struct request {
    grant capability;
    id request_id=0;
    method operation=method::version;
};
struct acknowledgement {
    transition installed;
    bool operator==(const acknowledgement& b) const { return installed==b.installed; }
};
}
