// test_pool.cpp — unit tests for warm-pool logic.
// Requires root + cgroups v2 + a valid rootfs with the workload binary.

#include "pool.h"
#include "cgroup.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
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

static bool can_run_sandbox() {
    return getuid() == 0 &&
           ws::cgroup::is_v2_available() &&
           fs::exists("/tmp/warmstart-rootfs/workload");
}

static ws::SandboxConfig test_cfg() {
    ws::SandboxConfig cfg;
    cfg.rootfs_path  = "/tmp/warmstart-rootfs";
    cfg.workload_bin = "/workload";
    cfg.weights_path = "";
    cfg.weights_size = 1 << 20;  // 1 MB
    cfg.mem_limit    = 512ULL << 20;
    return cfg;
}

TEST(pool_creation) {
    if (!can_run_sandbox()) SKIP("needs root + cgroup v2 + rootfs");

    ws::WarmPool pool(test_cfg(), 2);
    assert(pool.available() == 2);
    pool.shutdown();
}

TEST(pool_acquire_reduces_count) {
    if (!can_run_sandbox()) SKIP("needs root + cgroup v2 + rootfs");

    ws::WarmPool pool(test_cfg(), 3);
    int before = pool.available();
    auto h = pool.acquire();
    int after = pool.available();
    assert(after == before - 1);
    ws::teardown(h);
    pool.shutdown();
}

TEST(pool_refill_after_acquire) {
    if (!can_run_sandbox()) SKIP("needs root + cgroup v2 + rootfs");

    ws::WarmPool pool(test_cfg(), 2);
    auto h = pool.acquire();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    assert(pool.available() >= 1);
    ws::teardown(h);
    pool.shutdown();
}

TEST(pool_shutdown_is_clean) {
    if (!can_run_sandbox()) SKIP("needs root + cgroup v2 + rootfs");

    {
        ws::WarmPool pool(test_cfg(), 2);
        pool.shutdown();
    }
    // No hang, no crash.
}

TEST(pool_acquire_and_go) {
    if (!can_run_sandbox()) SKIP("needs root + cgroup v2 + rootfs");

    ws::WarmPool pool(test_cfg(), 2);
    auto h = pool.acquire();
    assert(h.go_fd >= 0);
    ws::send_go(h);
    double wait = ws::wait_ready(h, 15000);
    assert(wait > 0);
    ws::teardown(h);
    pool.shutdown();
}

int main() {
    std::cout << "Running pool tests:\n";
    std::cout << "\n" << tests_passed << "/" << tests_run << " passed";
    if (tests_skipped > 0) std::cout << " (" << tests_skipped << " skipped)";
    std::cout << "\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
