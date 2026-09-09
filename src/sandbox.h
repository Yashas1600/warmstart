#pragma once
// sandbox.h — low-level namespace / mount isolation primitives.
// Uses clone() with PID|MNT|UTS|IPC|NET namespaces and pivot_root.
// Linux-only: will not compile on other platforms.

#include <atomic>
#include <cstdint>
#include <string>
#include <chrono>
#include <sys/types.h>

namespace ws {

// Timing info for individual startup phases (parent-side measurements).
struct PhaseTimings {
    double ns_create_ms   = 0;  // namespace creation (clone syscall)
    double cgroup_ms      = 0;  // cgroup setup
    double total_ms       = 0;  // end-to-end (parent-side)
};

struct SandboxConfig {
    std::string rootfs_path  = "/tmp/warmstart-rootfs";
    std::string workload_bin = "/workload";       // path inside rootfs
    std::string weights_path;                      // host path to weights file
    uint64_t    weights_size = 1ULL << 30;         // 1 GB default
    uint64_t    mem_limit    = 2ULL << 30;         // cgroup mem limit
    int         cpu_weight   = 100;                // cgroup cpu.weight
    bool        prefault     = false;              // MAP_POPULATE / MADV_WILLNEED
    bool        hugepages    = false;              // MADV_HUGEPAGE
    bool        parked       = false;              // warm-pool: block before exec
};

struct SandboxHandle {
    pid_t       pid          = -1;
    int         ready_fd     = -1;   // read-end of pipe; workload writes 'R' on fd 3
    int         go_fd        = -1;   // write-end of go pipe (parked sandboxes only)
    std::string cgroup_path;
    PhaseTimings timings;
};

// Global atomic counter for unique sandbox names.
inline std::atomic<int> sandbox_counter{0};

// Launch a fully isolated sandbox from scratch.
// If cfg.parked is true, the child blocks after mount setup before exec.
SandboxHandle launch_cold(const SandboxConfig& cfg);

// Wait for the workload to signal ready ('R' on the pipe). Returns wait time in ms.
double wait_ready(const SandboxHandle& h, int timeout_ms = 30000);

// Send go signal to unpark a parked sandbox (triggers exec of workload).
void send_go(SandboxHandle& h);

// Tear down sandbox: kill process, close fds, remove cgroup.
void teardown(SandboxHandle& h);

}  // namespace ws
