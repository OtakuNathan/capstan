capstan_test(authority_faults_test tests/unit/authority_faults.cpp)
capstan_test(benchmark_policy_test tests/unit/benchmark_policy.cpp)
target_compile_definitions(benchmark_policy_test PRIVATE CAPSTAN_BENCHMARK_BUILD=1)
add_test(NAME negative_production_benchmark COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/compile_negative.py"
    --compiler "${CMAKE_CXX_COMPILER}" --source "${PROJECT_SOURCE_DIR}/tests/compile/production_benchmark.cpp"
    --root "${PROJECT_SOURCE_DIR}" --build "${PROJECT_BINARY_DIR}" --name production_benchmark)
capstan_test(wire_test tests/unit/wire.cpp)
if(NOT CAPSTAN_NO_EXCEPTIONS)
    capstan_test(authority_allocation_test tests/unit/authority_allocation.cpp)
else()
    message(STATUS "authority_allocation_test: throwing allocation rollback is EH-only; see checked_errors_test and allocation_test")
endif()
