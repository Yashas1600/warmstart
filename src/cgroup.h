#pragma once
// cgroup.h — cgroups v2 helpers: create, configure, add pid, destroy.

#include <cstdint>
#include <string>
#include <sys/types.h>

namespace ws::cgroup {

// Base path for the cgroup v2 unified hierarchy.
inline constexpr const char* kCgroupBase = "/sys/fs/cgroup";

// Create a cgroup under <base>/warmstart/<name>.
// Returns the full path (e.g. /sys/fs/cgroup/warmstart/sb-0).
std::string create(const std::string& name);

// Set memory.max (bytes) and cpu.weight on the cgroup at `path`.
void configure(const std::string& path, uint64_t mem_max, int cpu_weight);

// Move `pid` into the cgroup.
void add_pid(const std::string& path, pid_t pid);

// Read current memory usage (memory.current) in bytes.
uint64_t read_memory_current(const std::string& path);

// Remove the cgroup directory (after all processes have exited).
void destroy(const std::string& path);

// Check whether cgroups v2 unified hierarchy is available.
bool is_v2_available();

}  // namespace ws::cgroup
