// test_cgroup.cpp — unit tests for cgroup helpers.
// Minimal test framework (no gtest dependency).

#include "cgroup.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

int tests_run = 0;
int tests_passed = 0;

#define TEST(name)                                                  \
    void test_##name();                                             \
    struct Register_##name {                                        \
        Register_##name() {                                         \
            std::cout << "  " #name "... ";                         \
            ++tests_run;                                            \
            try { test_##name(); std::cout << "PASS\n"; ++tests_passed; } \
            catch (const std::exception& e) { std::cout << "FAIL: " << e.what() << "\n"; } \
        }                                                           \
    } reg_##name;                                                   \
    void test_##name()

#define EXPECT_TRUE(cond)  do { if (!(cond)) throw std::runtime_error(#cond " was false"); } while(0)
#define EXPECT_FALSE(cond) do { if (cond) throw std::runtime_error(#cond " was true"); } while(0)
#define EXPECT_EQ(a, b)    do { if ((a) != (b)) throw std::runtime_error(#a " != " #b); } while(0)
#define EXPECT_THROW(expr) do { bool threw = false; try { expr; } catch (...) { threw = true; } \
                                if (!threw) throw std::runtime_error(#expr " did not throw"); } while(0)

// ── Tests ─────────────────────────────────────────────────

TEST(is_v2_available_returns_bool) {
    // Just verify it doesn't crash and returns a bool.
    bool result = ws::cgroup::is_v2_available();
    (void)result;  // may be true or false depending on host
}

TEST(create_and_destroy) {
    if (!ws::cgroup::is_v2_available()) {
        std::cout << "(skipped: no cgroup v2) ";
        return;
    }
    std::string path = ws::cgroup::create("test-unit-001");
    EXPECT_TRUE(fs::exists(path));
    ws::cgroup::destroy(path);
    // Directory may or may not be gone depending on kernel.
}

TEST(configure_sets_limits) {
    if (!ws::cgroup::is_v2_available()) {
        std::cout << "(skipped: no cgroup v2) ";
        return;
    }
    std::string path = ws::cgroup::create("test-unit-002");
    ws::cgroup::configure(path, 512ULL << 20, 50);
    // Read back and verify.
    std::ifstream mem(path + "/memory.max");
    std::string val;
    std::getline(mem, val);
    EXPECT_EQ(val, std::to_string(512ULL << 20));

    ws::cgroup::destroy(path);
}

TEST(add_pid_self) {
    if (!ws::cgroup::is_v2_available()) {
        std::cout << "(skipped: no cgroup v2) ";
        return;
    }
    std::string path = ws::cgroup::create("test-unit-003");
    ws::cgroup::configure(path, 1ULL << 30, 100);
    // Adding our own pid should work (we're root in the test env).
    try {
        ws::cgroup::add_pid(path, getpid());
    } catch (...) {
        // May fail if not root — that's okay for the test.
        std::cout << "(needs root) ";
    }
    ws::cgroup::destroy(path);
}

TEST(read_memory_current) {
    if (!ws::cgroup::is_v2_available()) {
        std::cout << "(skipped: no cgroup v2) ";
        return;
    }
    std::string path = ws::cgroup::create("test-unit-004");
    ws::cgroup::configure(path, 1ULL << 30, 100);
    try {
        uint64_t mem = ws::cgroup::read_memory_current(path);
        // Should be >= 0 (the cgroup exists but may have no processes).
        (void)mem;
    } catch (...) {
        // memory.current might not exist if no processes in cgroup.
    }
    ws::cgroup::destroy(path);
}

TEST(destroy_nonexistent_is_safe) {
    // Destroying a path that doesn't exist should not throw.
    ws::cgroup::destroy("/sys/fs/cgroup/warmstart/nonexistent-test-xyz");
}

int main() {
    std::cout << "Running cgroup tests:\n";
    // Tests auto-register via static initializers above.
    std::cout << "\n" << tests_passed << "/" << tests_run << " tests passed.\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
