// sandbox.cpp — namespace creation, rootfs pivot, and sandbox lifecycle.

#include "sandbox.h"
#include "cgroup.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <signal.h>
#include <stdexcept>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// Linux-specific headers — project only targets Linux.
#ifdef __linux__
#include <sched.h>
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace ws {

namespace {

static int sandbox_counter = 0;

double ms_since(Clock::time_point start) {
    auto now = Clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}

#ifdef __linux__
// pivot_root is not wrapped by glibc on all distros.
int pivot_root(const char* new_root, const char* put_old) {
    return syscall(SYS_pivot_root, new_root, put_old);
}

// Child function executed after clone().
// Sets up mounts, pivot_root, then execs the workload.
int child_fn(void* arg) {
    auto* cfg = static_cast<SandboxConfig*>(arg);

    // ── Mount rootfs ──────────────────────────────────────
    // Bind-mount rootfs onto itself so it becomes a mount point.
    if (mount(cfg->rootfs_path.c_str(), cfg->rootfs_path.c_str(),
              nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        _exit(100);
    }

    // Create put_old directory inside new root.
    std::string put_old = cfg->rootfs_path + "/.old_root";
    mkdir(put_old.c_str(), 0700);

    if (pivot_root(cfg->rootfs_path.c_str(), put_old.c_str()) != 0) {
        _exit(101);
    }

    if (chdir("/") != 0) _exit(102);

    // Unmount old root.
    umount2("/.old_root", MNT_DETACH);
    rmdir("/.old_root");

    // Mount /proc for the new PID namespace.
    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, nullptr);

    // Mount /dev as tmpfs.
    mkdir("/dev", 0755);
    mount("tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "size=64k");

    // Set hostname.
    sethostname("warmstart", 9);

    // Exec the workload.
    // Pass weights info via environment variables.
    std::string wsize = std::to_string(cfg->weights_size);
    std::string pf    = cfg->prefault ? "1" : "0";
    std::string hp    = cfg->hugepages ? "1" : "0";

    setenv("WS_WEIGHTS_SIZE", wsize.c_str(), 1);
    setenv("WS_PREFAULT", pf.c_str(), 1);
    setenv("WS_HUGEPAGES", hp.c_str(), 1);
    if (!cfg->weights_path.empty()) {
        setenv("WS_WEIGHTS_PATH", cfg->weights_path.c_str(), 1);
    }

    const char* argv[] = {cfg->workload_bin.c_str(), nullptr};
    execv(cfg->workload_bin.c_str(), const_cast<char* const*>(argv));

    // If exec fails, exit with error.
    _exit(103);
}
#endif  // __linux__

}  // namespace

SandboxHandle launch_cold(const SandboxConfig& cfg) {
    SandboxHandle h;
    PhaseTimings& t = h.timings;
    auto t0 = Clock::now();

#ifdef __linux__
    // ── Phase 1: Create cgroup ────────────────────────────
    auto cg_start = Clock::now();
    std::string name = "sb-" + std::to_string(sandbox_counter++);
    h.cgroup_path = cgroup::create(name);
    cgroup::configure(h.cgroup_path, cfg.mem_limit, cfg.cpu_weight);
    t.cgroup_ms = ms_since(cg_start);

    // ── Phase 2: Create pipe for ready signal ─────────────
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
        throw std::runtime_error("pipe2 failed: " + std::string(std::strerror(errno)));
    }

    // ── Phase 3: Clone with new namespaces ────────────────
    auto ns_start = Clock::now();
    const int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
                            CLONE_NEWIPC | CLONE_NEWNET | SIGCHLD;
    constexpr size_t kStackSize = 1024 * 1024;  // 1 MiB child stack
    auto* stack = static_cast<char*>(malloc(kStackSize));
    if (!stack) throw std::runtime_error("malloc stack failed");

    // We need a mutable copy of cfg for the child.
    auto cfg_copy = cfg;

    pid_t pid = clone(child_fn, stack + kStackSize, clone_flags, &cfg_copy);
    if (pid < 0) {
        free(stack);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        throw std::runtime_error("clone failed: " + std::string(std::strerror(errno)));
    }
    t.ns_create_ms = ms_since(ns_start);

    // Move child into cgroup.
    cgroup::add_pid(h.cgroup_path, pid);

    h.pid = pid;
    h.ready_fd = pipe_fds[0];
    close(pipe_fds[1]);  // close write end in parent
    free(stack);
#else
    // Stub for non-Linux: simulate latency for development.
    (void)cfg;
    h.pid = -1;
    h.ready_fd = -1;
    t.ns_create_ms = 0.5;
    t.cgroup_ms    = 0.3;
    t.mount_ms     = 1.2;
    t.exec_ms      = 0.8;
    t.weight_load_ms = 15.0;
#endif

    t.total_ms = ms_since(t0);
    return h;
}

SandboxHandle launch_from_fork(const SandboxConfig& cfg, pid_t template_pid) {
    // Used by zygote: the template_pid's memory is inherited via fork().
    SandboxHandle h;

#ifdef __linux__
    auto t0 = Clock::now();

    // Signal the template to fork (via pipe, handled in zygote.cpp).
    // This function is typically called from Zygote::fork_child(),
    // which manages the IPC. Here we just record timings.
    h.timings.total_ms = ms_since(t0);
#else
    (void)cfg;
    (void)template_pid;
#endif

    return h;
}

double wait_ready(const SandboxHandle& h, int timeout_ms) {
    if (h.ready_fd < 0) return 0.0;

    auto start = Clock::now();

#ifdef __linux__
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(h.ready_fd, &fds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(h.ready_fd + 1, &fds, nullptr, nullptr, &tv);
    if (ret <= 0) {
        throw std::runtime_error("workload did not signal ready within timeout");
    }

    char buf;
    ssize_t n = read(h.ready_fd, &buf, 1);
    if (n != 1 || buf != 'R') {
        throw std::runtime_error("unexpected ready signal from workload");
    }
#endif

    auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void teardown(SandboxHandle& h) {
#ifdef __linux__
    if (h.pid > 0) {
        kill(h.pid, SIGKILL);
        waitpid(h.pid, nullptr, 0);
        h.pid = -1;
    }
    if (h.ready_fd >= 0) {
        close(h.ready_fd);
        h.ready_fd = -1;
    }
    if (!h.cgroup_path.empty()) {
        cgroup::destroy(h.cgroup_path);
        h.cgroup_path.clear();
    }
#else
    (void)h;
#endif
}

}  // namespace ws
