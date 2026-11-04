#include <capstan/authority/host.hpp>
int main() {
    capstan::host_authority host({1,2,3},4,
        capstan::host_authority::benchmark_validation::omit_grant_checks);
}
