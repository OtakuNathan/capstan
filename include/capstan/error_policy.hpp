#pragma once
#include <cassert>
#include <cstdlib>
#include <exception>
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define CAPSTAN_HAS_EXCEPTIONS 1
#define CAPSTAN_TRY try
#define CAPSTAN_CATCH_ALL catch(...)
#define CAPSTAN_CATCH_STD(e) catch(const std::exception& e)
#define CAPSTAN_RAISE(error) throw error
#define CAPSTAN_RETHROW throw
#else
#define CAPSTAN_HAS_EXCEPTIONS 0
namespace capstan { inline const std::exception& disabled_exception() { static const std::exception e; return e; } }
// Allocation/unexpected-exception recovery is retained in exception builds.
// Recoverable input errors use checked APIs in BOTH builds. These macros never
// turn network validation into an assertion or silently manufacture success.
#define CAPSTAN_TRY if(true)
#define CAPSTAN_CATCH_ALL else if(false)
#define CAPSTAN_CATCH_STD(e) else if(false) for(const auto& e=::capstan::disabled_exception(); false;)
#define CAPSTAN_RAISE(error) do { assert(false && #error); std::abort(); } while(false)
#define CAPSTAN_RETHROW std::abort()
#endif
