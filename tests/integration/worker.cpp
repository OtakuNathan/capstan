#include "src/apps/worker.hpp"
#include "tests/check.hpp"
#include <atomic>
#include <thread>

int main() {
    const auto owner = std::this_thread::get_id();
    for (int round = 0; round < 20; ++round) {
        std::atomic<int> executed{0};
        int accepted = 0;
        {
            app::business_worker worker;
            for (int i = 0; i < 300; ++i) {
                flux_foundry::task_wrapper_sbo task([&]() noexcept {
                    CHECK(std::this_thread::get_id() != owner);
                    ++executed;
                });
                if (worker.try_dispatch(std::move(task))) ++accepted;
                else CHECK(task); // A rejected task never executes later.
            }
            if (round % 2) { worker.close(); worker.close(); }
            // Other rounds exercise destructor-driven drain and join.
        }
        CHECK(accepted > 0 && executed == accepted);
    }
}
