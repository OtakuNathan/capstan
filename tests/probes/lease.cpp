#include <lease.hpp>
#include <memory>
#include "tests/check.hpp"
struct referent {
    int* deaths;
    int value() const noexcept { return 7; }
    ~referent() { ++*deaths; }
};
struct owner {
    std::shared_ptr<referent> backing;
    lease::access::exclusive_access<referent, lease::access::shared> access;
    explicit owner(int& deaths) : backing(new referent{&deaths}), access(lease::access::make_rw<lease::access::shared>(*backing)) {}
    owner(owner&&) noexcept = default;
};
int main() {
    int deaths = 0;
    {
        owner a(deaths);
        auto* stable = a.backing.get();
        owner b(std::move(a));
        CHECK(b.backing.get() == stable);
        {
            // Operation frame owns both backing lifetime and a real read lease.
            struct frame {
                std::shared_ptr<referent> backing;
                lease::access::shared_access<referent, lease::access::shared> read;
            };
            frame op{b.backing, b.access.borrow_ro()};
            auto copy = op.read;
            CHECK(copy->value() == 7 && deaths == 0);
            CHECK(op.read.operator->() == stable);
        }
        CHECK(deaths == 0);
    }
    CHECK(deaths == 1);
    std::cout << "stable referent across owner move; read copies; reverse member destruction passed\n";
}
