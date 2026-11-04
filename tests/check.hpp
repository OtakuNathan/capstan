#pragma once
#include <cstdlib>
#include <iostream>
#include <exception>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(expr) do { if (!(expr)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #expr "\n"; std::abort(); } } while (false)
// Legacy must-succeed APIs throw with EH enabled and terminate on contract
// violation without EH. Test both in Release; assert disappearing is not enough.
template<class Error,class F> bool rejects(F f) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    try {f();} catch(const Error&) {return true;}
    return false;
#else
    auto child=fork();CHECK(child>=0);
    if(child==0) {f();_exit(0);}
    int state=0;CHECK(waitpid(child,&state,0)==child);
    return WIFSIGNALED(state) && WTERMSIG(state)==SIGABRT;
#endif
}
