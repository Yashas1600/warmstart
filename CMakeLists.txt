cmake_minimum_required(VERSION 3.20)
project(warmstart VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Main launcher ─────────────────────────────────────────────
add_executable(warmstart
    src/main.cpp
    src/launcher.cpp
    src/cgroup.cpp
    src/sandbox.cpp
    src/pool.cpp
    src/zygote.cpp
    src/benchmark.cpp
)
target_include_directories(warmstart PRIVATE src)
target_link_libraries(warmstart PRIVATE pthread)

# ── Dummy workload ────────────────────────────────────────────
add_executable(dummy_workload workload/dummy_workload.cpp)

# ── Tests ─────────────────────────────────────────────────────
enable_testing()

add_executable(test_cgroup tests/test_cgroup.cpp src/cgroup.cpp)
target_include_directories(test_cgroup PRIVATE src)
add_test(NAME cgroup_tests COMMAND test_cgroup)

add_executable(test_pool tests/test_pool.cpp src/pool.cpp src/sandbox.cpp src/cgroup.cpp)
target_include_directories(test_pool PRIVATE src)
target_link_libraries(test_pool PRIVATE pthread)
add_test(NAME pool_tests COMMAND test_pool)
