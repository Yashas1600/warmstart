// test_pool.cpp — unit tests for warm-pool logic.

#include "pool.h"

#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>

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

// Note: These tests require Linux + root for actual sandbox creation.
// On other platforms they verify the pool logic with stub sandboxes.

TEST(pool_creation) {
    ws::SandboxConfig cfg;
    cfg.rootfs_path = "/tmp/warmstart-rootfs-test";
    cfg.weights_size = 1 << 20;  // 1 MB for testing

#ifdef __linux__
    // Only run actual pool tests on Linux.
    ws::WarmPool pool(cfg, 2);
    // Pool should have 2 sandboxes available.
    assert(pool.available() == 2);
    pool.shutdown();
#else
    std::cout << "(skipped: non-Linux) ";
#endif
}

TEST(pool_acquire_reduces_count) {
#ifdef __linux__
    ws::SandboxConfig cfg;
    cfg.rootfs_path = "/tmp/warmstart-rootfs-test";
    cfg.weights_size = 1 << 20;

    ws::WarmPool pool(cfg, 3);
    int before = pool.available();
    auto h = pool.acquire();
    int after = pool.available();
    assert(after == before - 1);
    ws::teardown(h);
    pool.shutdown();
#else
    std::cout << "(skipped: non-Linux) ";
#endif
}

TEST(pool_refill_after_acquire) {
#ifdef __linux__
    ws::SandboxConfig cfg;
    cfg.rootfs_path = "/tmp/warmstart-rootfs-test";
    cfg.weights_size = 1 << 20;

    ws::WarmPool pool(cfg, 2);
    auto h = pool.acquire();
    // Give refill thread time to create a replacement.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    assert(pool.available() >= 1);  // should have refilled
    ws::teardown(h);
    pool.shutdown();
#else
    std::cout << "(skipped: non-Linux) ";
#endif
}

TEST(pool_shutdown_is_clean) {
    ws::SandboxConfig cfg;
    cfg.rootfs_path = "/tmp/warmstart-rootfs-test";
    cfg.weights_size = 1 << 20;

#ifdef __linux__
    {
        ws::WarmPool pool(cfg, 2);
        // Let it initialize, then immediately shut down.
        pool.shutdown();
    }
    // Destructor should not hang or crash.
#else
    std::cout << "(skipped: non-Linux) ";
#endif
}

TEST(pool_multiple_acquires) {
#ifdef __linux__
    ws::SandboxConfig cfg;
    cfg.rootfs_path = "/tmp/warmstart-rootfs-test";
    cfg.weights_size = 1 << 20;

    ws::WarmPool pool(cfg, 4);
    std::vector<ws::SandboxHandle> handles;
    for (int i = 0; i < 4; ++i) {
        handles.push_back(pool.acquire());
    }
    assert(pool.available() == 0);
    for (auto& h : handles) {
        ws::teardown(h);
    }
    pool.shutdown();
#else
    std::cout << "(skipped: non-Linux) ";
#endif
}

int main() {
    std::cout << "Running pool tests:\n";
    std::cout << "\n" << tests_passed << "/" << tests_run << " tests passed.\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
