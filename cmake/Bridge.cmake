capstan_test(loopback_test tests/integration/loopback.cpp)
capstan_test(callback_test tests/integration/callback.cpp)
add_test(NAME p2_compile_negative COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/p2_compile_negative.py"
    --compiler "${CMAKE_CXX_COMPILER}" --root "${PROJECT_SOURCE_DIR}" --build "${PROJECT_BINARY_DIR}")
capstan_test(allocation_test tests/integration/allocation.cpp)
