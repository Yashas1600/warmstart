// cgroup.cpp — cgroups v2 manipulation for sandbox resource limits.

#include "cgroup.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace ws::cgroup {

namespace {

void write_file(const std::string& path, const std::string& value) {
    std::ofstream f(path);
    if (!f) {
        throw std::runtime_error("cgroup: cannot open " + path + ": " +
                                 std::strerror(errno));
    }
    f << value;
    if (!f) {
        throw std::runtime_error("cgroup: write failed to " + path);
    }
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

}  // namespace

std::string create(const std::string& name) {
    std::string base = std::string(kCgroupBase) + "/warmstart";
    std::string path = base + "/" + name;

    // Ensure parent group exists with controllers delegated.
    fs::create_directories(base);

    // Enable cpu and memory controllers on parent.
    std::string subtree = base + "/cgroup.subtree_control";
    if (fs::exists(subtree)) {
        // Best-effort: may already be set.
        std::ofstream f(subtree);
        if (f) f << "+cpu +memory";
    }

    fs::create_directories(path);
    return path;
}

void configure(const std::string& path, uint64_t mem_max, int cpu_weight) {
    write_file(path + "/memory.max", std::to_string(mem_max));
    write_file(path + "/cpu.weight", std::to_string(cpu_weight));
}

void add_pid(const std::string& path, pid_t pid) {
    write_file(path + "/cgroup.procs", std::to_string(pid));
}

uint64_t read_memory_current(const std::string& path) {
    std::string val = read_file(path + "/memory.current");
    return std::stoull(val);
}

void destroy(const std::string& path) {
    // rmdir only works when empty (no child cgroups, no processes).
    std::error_code ec;
    fs::remove(path, ec);
    // Silently ignore errors — the cgroup may still have zombie processes.
}

bool is_v2_available() {
    return fs::exists(std::string(kCgroupBase) + "/cgroup.controllers");
}

}  // namespace ws::cgroup
