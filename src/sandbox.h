#pragma once
// sandbox.h — low-level namespace / mount isolation primitives
// Uses clone() with PID|MNT|UTS|IPC|NET namespaces and pivot_root.

#include <cstdint>
#include <string>
#include <functional>
#include <chrono>

namespace ws {

// Timing info for individual startup phases
struct PhaseTimings {
    double ns_create_ms   = 0;  // namespace creation
    double cgroup_ms      = 0;  // cgroup setup
    double mount_ms       = 0;  // rootfs mount + pivot_root
    double exec_ms        = 0;  // exec of workload
    double weight_load_ms = 0;  // mmap + touch of weights file
    double total_ms       = 0;  // end-to-end
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
};

struct SandboxHandle {
    pid_t       pid          = -1;
    int         ready_fd     = -1;   // read-end of pipe; workload writes 'R'
    std::string cgroup_path;
    PhaseTimings timings;
};

// Launch a fully isolated sandbox from scratch (cold path).
SandboxHandle launch_cold(const SandboxConfig& cfg);

// Fork from inside an already-set-up namespace (used by pool & zygote).
SandboxHandle launch_from_fork(const SandboxConfig& cfg, pid_t template_pid);

// Wait for the workload to signal ready on the pipe. Returns latency in ms.
double wait_ready(const SandboxHandle& h, int timeout_ms = 30000);

// Tear down sandbox: kill process, remove cgroup, unmount.
void teardown(SandboxHandle& h);

}  // namespace ws
