#pragma once
#include <capstan/error_policy.hpp>
#include <capstan/protocol/types.hpp>
#include <cerrno>
#include <stdexcept>
#include <sys/random.h>
namespace capstan {
inline bool try_os_random(void* destination,std::size_t size) {
    auto p=static_cast<unsigned char*>(destination);
    while(size) {
        auto n=::getrandom(p,size,0);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) return false;
        p+=n; size-=static_cast<std::size_t>(n);
    }
    return true;
}
inline void os_random(void* destination,std::size_t size) {if(!try_os_random(destination,size)) CAPSTAN_RAISE(std::runtime_error("OS random source failed"));}
inline bool try_random_token(token& t) {do {if(!try_os_random(t.data(),t.size())) return false;} while(t==token{});return true;}
inline bool try_random_id(id& n) {do {if(!try_os_random(&n,sizeof(n))) return false;} while(!n);return true;}
inline token random_token() { token t; do { os_random(t.data(),t.size()); } while(t==token{}); return t; }
inline id random_id() { id n; do { os_random(&n,sizeof(n)); } while(!n); return n; }
}
