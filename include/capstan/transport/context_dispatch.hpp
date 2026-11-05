#pragma once
#include <capstan/error_policy.hpp>
#include <glib.h>
#include <atomic>
#include <exception>
#include <functional>
#include <utility>

namespace capstan {
namespace context_detail {
struct posted_work {
    std::atomic<bool> published{false};
    std::function<void()> work;
    explicit posted_work(std::function<void()> action):work(std::move(action)) {
        published.store(true,std::memory_order_release);
    }
    void acquire() const noexcept {
        if(!published.load(std::memory_order_acquire)) std::terminate();
    }
    ~posted_work() { acquire(); }
};
}

// Always enqueue on the context; never borrow an unowned context and execute
// inline on the submitting worker. The explicit publication edge covers both
// the closure and preceding business writes, including with uninstrumented
// GLib builds. Destruction also acquires if the context discards the source.
inline void post_context(GMainContext* context,std::function<void()> work) {
    auto* payload=new context_detail::posted_work(std::move(work));
    auto* source=g_idle_source_new();
    g_source_set_callback(source,[](gpointer data)->gboolean {
        auto* task=static_cast<context_detail::posted_work*>(data);
        task->acquire();
        CAPSTAN_TRY {task->work();}
        CAPSTAN_CATCH_ALL {std::terminate();}
        return G_SOURCE_REMOVE;
    },payload,[](gpointer data){delete static_cast<context_detail::posted_work*>(data);});
    g_source_attach(source,context);
    g_source_unref(source);
}
}
