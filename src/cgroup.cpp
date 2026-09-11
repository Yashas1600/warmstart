// cgroup.cpp — cgroups v2 manipulation for sandbox resource limits.

#include "cgroup.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace ws::cgroup {

namespace {

// Write to a cgroup file using raw write(2) — required because some cgroup
// files need an atomic single-write (e.g., cgroup.procs, subtree_control).
void raw_write(const std::string& path, const std::string& value) {
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        throw std::runtime_error("cgroup: cannot open " + path + ": " +
                                 std::strerror(errno));
    }
    ssize_t n = write(fd, value.c_str(), value.size());
    int err = errno;
    close(fd);
    if (n < 0) {
        throw std::runtime_error("cgroup: write failed to " + path + ": " +
                                 std::strerror(err));
    }
}

// Best-effort raw write — ignore failures.
void try_raw_write(const std::string& path, const std::string& value) {
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) return;
    (void)write(fd, value.c_str(), value.size());
    close(fd);
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("cgroup: cannot read " + path + ": " +
                                 std::strerror(errno));
    }
    std::string val;
    std::getline(f, val);
    return val;
}

// Ensure the root cgroup has cpu+memory controllers delegated.
// In Docker containers, the root cgroup often has processes (PID 1, us, etc.).
// subtree_control can only be written when there are no processes directly
// in the cgroup, so we move ourselves to a holding group first.
void ensure_root_delegation() {
    static bool done = false;
    if (done) return;
    done = true;

    std::string root = std::string(kCgroupBase);
    std::string subtree = root + "/cgroup.subtree_control";

    // Check if controllers already delegated.
    {
        std::ifstream f(subtree);
        std::string current;
        if (f) std::getline(f, current);
        if (current.find("memory") != std::string::npos &&
            current.find("cpu") != std::string::npos) {
            return;
        }
    }

    // Create a holding cgroup and move ALL processes out of root into it.
    // subtree_control can only be written when no processes are directly
    // in the cgroup. Docker containers often have PID 1 and shells here.
    std::string init_cg = root + "/ws-init";
    fs::create_directories(init_cg);

    // Read all PIDs in root cgroup and move them.
    {
        std::ifstream procs(root + "/cgroup.procs");
        std::string pid_str;
        while (std::getline(procs, pid_str)) {
            if (!pid_str.empty()) {
                try_raw_write(init_cg + "/cgroup.procs", pid_str);
            }
        }
    }

    // Now root cgroup should be empty — enable controllers.
    try_raw_write(subtree, "+cpu +memory");
}

}  // namespace

std::string create(const std::string& name) {
    ensure_root_delegation();

    std::string base = std::string(kCgroupBase) + "/warmstart";
    std::string path = base + "/" + name;

    fs::create_directories(base);

    // Enable controllers for sub-groups.
    std::string subtree = base + "/cgroup.subtree_control";
    if (fs::exists(subtree)) {
        try_raw_write(subtree, "+cpu +memory");
    }

    fs::create_directories(path);
    return path;
}

void configure(const std::string& path, uint64_t mem_max, int cpu_weight) {
    raw_write(path + "/memory.max", std::to_string(mem_max));
    raw_write(path + "/cpu.weight", std::to_string(cpu_weight));
}

void add_pid(const std::string& path, pid_t pid) {
    raw_write(path + "/cgroup.procs", std::to_string(pid));
}

uint64_t read_memory_current(const std::string& path) {
    std::string val = read_file(path + "/memory.current");
    return std::stoull(val);
}

void destroy(const std::string& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

bool is_v2_available() {
    return fs::exists(std::string(kCgroupBase) + "/cgroup.controllers");
}

}  // namespace ws::cgroup
