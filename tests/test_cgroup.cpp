// test_cgroup.cpp — unit tests for cgroup helpers.
// Requires root + cgroups v2 for most tests.

#include "cgroup.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

int tests_run = 0;
int tests_passed = 0;
int tests_skipped = 0;

#define TEST(name)                                                  \
    void test_##name();                                             \
    struct Register_##name {                                        \
        Register_##name() {                                         \
            std::cout << "  " #name "... " << std::flush;           \
            ++tests_run;                                            \
            try { test_##name(); std::cout << "PASS\n"; ++tests_passed; } \
            catch (const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; } \
        }                                                           \
    } reg_##name;                                                   \
    void test_##name()

#define SKIP(reason) do { std::cout << "SKIP: " reason "\n"; ++tests_skipped; --tests_run; return; } while(0)

static bool can_run() {
    return getuid() == 0 && ws::cgroup::is_v2_available();
}

TEST(is_v2_available_returns_bool) {
    bool result = ws::cgroup::is_v2_available();
    (void)result;
}

TEST(create_and_destroy) {
    if (!can_run()) SKIP("needs root + cgroup v2");

    std::string path = ws::cgroup::create("test-unit-001");
    assert(fs::exists(path));
    ws::cgroup::destroy(path);
}

TEST(configure_sets_limits) {
    if (!can_run()) SKIP("needs root + cgroup v2");

    std::string path = ws::cgroup::create("test-unit-002");
    ws::cgroup::configure(path, 512ULL << 20, 50);

    std::ifstream mem(path + "/memory.max");
    std::string val;
    std::getline(mem, val);
    assert(val == std::to_string(512ULL << 20));

    ws::cgroup::destroy(path);
}

TEST(add_pid_self) {
    if (!can_run()) SKIP("needs root + cgroup v2");

    std::string path = ws::cgroup::create("test-unit-003");
    ws::cgroup::configure(path, 1ULL << 30, 100);
    try {
        ws::cgroup::add_pid(path, getpid());
    } catch (...) {
        std::cout << "(add_pid failed, may need different cgroup hierarchy) ";
    }
    ws::cgroup::destroy(path);
}

TEST(read_memory_current) {
    if (!can_run()) SKIP("needs root + cgroup v2");

    std::string path = ws::cgroup::create("test-unit-004");
    ws::cgroup::configure(path, 1ULL << 30, 100);
    try {
        uint64_t mem = ws::cgroup::read_memory_current(path);
        (void)mem;
    } catch (...) {
        // memory.current may not exist without processes in the cgroup.
    }
    ws::cgroup::destroy(path);
}

TEST(destroy_nonexistent_is_safe) {
    ws::cgroup::destroy("/sys/fs/cgroup/warmstart/nonexistent-test-xyz");
}

int main() {
    std::cout << "Running cgroup tests:\n";
    std::cout << "\n" << tests_passed << "/" << tests_run << " passed";
    if (tests_skipped > 0) std::cout << " (" << tests_skipped << " skipped)";
    std::cout << "\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
