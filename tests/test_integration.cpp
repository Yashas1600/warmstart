// test_integration.cpp — integration test: run each strategy 5 times,
// assert ready signal is received, verify no zombie processes remain.

#include "benchmark.h"
#include "cgroup.h"
#include "pool.h"
#include "sandbox.h"
#include "zygote.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

static int tests_run = 0;
static int tests_passed = 0;
static int tests_skipped = 0;

#define TEST(name)                                                  \
    void test_##name();                                             \
    struct Register_##name {                                        \
        Register_##name() {                                         \
            std::cout << "  " #name "... " << std::flush;           \
            ++tests_run;                                            \
            try {                                                   \
                test_##name();                                      \
                std::cout << "PASS\n";                              \
                ++tests_passed;                                     \
            } catch (const std::exception& e) {                     \
                std::cout << "FAIL: " << e.what() << "\n";          \
            }                                                       \
        }                                                           \
    } reg_##name;                                                   \
    void test_##name()

#define SKIP(reason) do { std::cout << "SKIP: " reason "\n"; ++tests_skipped; --tests_run; return; } while(0)

static bool prerequisites_met() {
    if (getuid() != 0) return false;
    if (!ws::cgroup::is_v2_available()) return false;
    if (!fs::exists("/tmp/warmstart-rootfs/workload")) return false;
    return true;
}

static ws::SandboxConfig test_config() {
    ws::SandboxConfig cfg;
    cfg.rootfs_path  = "/tmp/warmstart-rootfs";
    cfg.workload_bin = "/workload";
    // Use small anonymous mmap for fast testing.
    cfg.weights_path = "";
    cfg.weights_size = 4ULL << 20;  // 4 MB
    cfg.mem_limit    = 512ULL << 20;
    cfg.cpu_weight   = 100;
    return cfg;
}

static int count_zombies() {
    int count = 0;
    while (true) {
        pid_t z = waitpid(-1, nullptr, WNOHANG);
        if (z <= 0) break;
        ++count;
    }
    return count;
}

// ── Tests ─────────────────────────────────────────────────

TEST(cold_start_5_runs) {
    if (!prerequisites_met()) SKIP("prerequisites not met");

    auto cfg = test_config();
    for (int i = 0; i < 5; ++i) {
        auto h = ws::launch_cold(cfg);
        assert(h.pid > 0);
        assert(h.ready_fd >= 0);
        double wait = ws::wait_ready(h, 15000);
        assert(wait > 0);
        ws::teardown(h);
    }
}

TEST(cold_no_zombies) {
    if (!prerequisites_met()) SKIP("prerequisites not met");

    int z = count_zombies();
    assert(z == 0);
}

TEST(warm_pool_5_runs) {
    if (!prerequisites_met()) SKIP("prerequisites not met");

    auto cfg = test_config();
    ws::WarmPool pool(cfg, 3);
    for (int i = 0; i < 5; ++i) {
        auto h = pool.acquire();
        assert(h.pid > 0);
        assert(h.go_fd >= 0);
        ws::send_go(h);
        double wait = ws::wait_ready(h, 15000);
        assert(wait > 0);
        ws::teardown(h);
    }
    pool.shutdown();
}

TEST(warm_pool_no_zombies) {
    if (!prerequisites_met()) SKIP("prerequisites not met");

    int z = count_zombies();
    assert(z == 0);
}

TEST(zygote_5_runs) {
    if (!prerequisites_met()) SKIP("prerequisites not met");

    auto cfg = test_config();
    ws::Zygote zyg(cfg);
    zyg.init();
    for (int i = 0; i < 5; ++i) {
        auto h = zyg.fork_child();
        assert(h.timings.total_ms > 0);
        // Child already exited and reaped — pid should be -1.
        assert(h.pid == -1);
    }
    zyg.shutdown();
}

TEST(zygote_no_zombies) {
    if (!prerequisites_met()) SKIP("prerequisites not met");

    int z = count_zombies();
    assert(z == 0);
}

int main() {
    std::cout << "Running integration tests:\n";
    if (!prerequisites_met()) {
        std::cout << "\n  Prerequisites not met. Need: root, cgroup v2, "
                     "/tmp/warmstart-rootfs/workload\n"
                  << "  Run: sudo bash scripts/build_rootfs.sh && "
                     "sudo cp build/dummy_workload /tmp/warmstart-rootfs/workload\n";
    }
    // Tests auto-register via static initializers above.
    std::cout << "\n" << tests_passed << "/" << tests_run << " passed";
    if (tests_skipped > 0) std::cout << " (" << tests_skipped << " skipped)";
    std::cout << "\n";
    return (tests_passed == tests_run) ? 0 : 1;
}
