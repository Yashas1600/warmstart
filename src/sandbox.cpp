// sandbox.cpp — namespace creation, rootfs pivot, and sandbox lifecycle.

#include "sandbox.h"
#include "cgroup.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;

namespace ws {

namespace {

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int pivot_root(const char* new_root, const char* put_old) {
    return syscall(SYS_pivot_root, new_root, put_old);
}

// Everything the clone child needs. Passed via void* arg.
// Safe because clone() without CLONE_VM gives a full COW address space copy.
struct ChildArgs {
    // Config (child gets a COW copy of parent's strings — safe to read).
    std::string rootfs_path;
    std::string workload_bin;
    std::string weights_path;
    uint64_t    weights_size;
    bool        prefault;
    bool        hugepages;

    // Pipe file descriptors.
    int ready_wr;   // write-end of ready pipe (dup2 to fd 3 before exec)
    int ready_rd;   // read-end of ready pipe (close in child)
    int sync_rd;    // read-end of sync pipe (blocks until parent finishes cgroup)
    int sync_wr;    // write-end of sync pipe (close in child)
    int go_rd;      // read-end of go pipe (-1 if not parked, blocks until acquire)
    int go_wr;      // write-end of go pipe (close in child)
};

// Clone child entry point.
int child_fn(void* arg) {
    auto* a = static_cast<ChildArgs*>(arg);

    // Close parent-side pipe ends.
    close(a->ready_rd);
    close(a->sync_wr);
    if (a->go_wr >= 0) close(a->go_wr);

    // ── Block until parent has added us to our cgroup ─────
    char sync_buf;
    if (read(a->sync_rd, &sync_buf, 1) != 1) {
        dprintf(STDERR_FILENO, "[child] sync pipe read failed\n");
        _exit(110);
    }
    close(a->sync_rd);

    // ── Make root mount private (required on systemd hosts) ──
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        dprintf(STDERR_FILENO, "[child] MS_REC|MS_PRIVATE on / failed: %s\n",
                strerror(errno));
        _exit(100);
    }

    // ── Bind-mount weights file into rootfs if specified ──
    if (!a->weights_path.empty()) {
        std::string mp = a->rootfs_path + "/weights.bin";
        int fd = open(mp.c_str(), O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) close(fd);
        if (mount(a->weights_path.c_str(), mp.c_str(), nullptr, MS_BIND, nullptr) != 0) {
            dprintf(STDERR_FILENO, "[child] weights bind-mount failed: %s\n",
                    strerror(errno));
            // Non-fatal: workload will fall back to anonymous mmap.
        }
    }

    // ── Bind-mount rootfs onto itself ─────────────────────
    if (mount(a->rootfs_path.c_str(), a->rootfs_path.c_str(),
              nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        dprintf(STDERR_FILENO, "[child] rootfs bind-mount failed: %s\n",
                strerror(errno));
        _exit(101);
    }

    // ── pivot_root ────────────────────────────────────────
    std::string put_old = a->rootfs_path + "/.old_root";
    mkdir(put_old.c_str(), 0700);
    if (pivot_root(a->rootfs_path.c_str(), put_old.c_str()) != 0) {
        dprintf(STDERR_FILENO, "[child] pivot_root failed: %s\n",
                strerror(errno));
        _exit(102);
    }
    if (chdir("/") != 0) {
        dprintf(STDERR_FILENO, "[child] chdir / failed: %s\n", strerror(errno));
        _exit(103);
    }

    // Unmount old root.
    if (umount2("/.old_root", MNT_DETACH) != 0) {
        dprintf(STDERR_FILENO, "[child] umount /.old_root failed: %s\n",
                strerror(errno));
    }
    rmdir("/.old_root");

    // ── Mount /proc and /dev ──────────────────────────────
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, nullptr) != 0) {
        dprintf(STDERR_FILENO, "[child] mount /proc failed: %s\n",
                strerror(errno));
    }
    mkdir("/dev", 0755);
    mount("tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "size=64k");

    // Set hostname.
    sethostname("warmstart", 9);

    // ── If parked (warm-pool), block until go signal ──────
    if (a->go_rd >= 0) {
        char go_buf;
        if (read(a->go_rd, &go_buf, 1) != 1) {
            // go_fd closed = sandbox being torn down without running.
            _exit(111);
        }
        close(a->go_rd);
    }

    // ── dup2 ready pipe write-end onto fd 3 ───────────────
    // dup2 clears O_CLOEXEC on the target fd, so fd 3 survives exec.
    if (dup2(a->ready_wr, 3) < 0) {
        dprintf(STDERR_FILENO, "[child] dup2 ready pipe to fd 3 failed: %s\n",
                strerror(errno));
        _exit(104);
    }
    close(a->ready_wr);  // original fd (with O_CLOEXEC) no longer needed

    // ── Set environment variables for workload ────────────
    std::string wsize = std::to_string(a->weights_size);
    std::string pf    = a->prefault ? "1" : "0";
    std::string hp    = a->hugepages ? "1" : "0";
    setenv("WS_WEIGHTS_SIZE", wsize.c_str(), 1);
    setenv("WS_PREFAULT", pf.c_str(), 1);
    setenv("WS_HUGEPAGES", hp.c_str(), 1);
    // After pivot_root, the bind-mounted weights file is at /weights.bin.
    setenv("WS_WEIGHTS_PATH",
           a->weights_path.empty() ? "" : "/weights.bin", 1);

    // ── Exec workload ─────────────────────────────────────
    const char* argv[] = {a->workload_bin.c_str(), nullptr};
    execv(a->workload_bin.c_str(), const_cast<char* const*>(argv));

    dprintf(STDERR_FILENO, "[child] execv(%s) failed: %s\n",
            a->workload_bin.c_str(), strerror(errno));
    _exit(105);
}

}  // namespace

SandboxHandle launch_cold(const SandboxConfig& cfg) {
    SandboxHandle h;
    auto t0 = Clock::now();

    // ── Phase 1: Create cgroup ────────────────────────────
    auto cg_start = Clock::now();
    int id = sandbox_counter.fetch_add(1);
    std::string name = "sb-" + std::to_string(id);
    h.cgroup_path = cgroup::create(name);
    cgroup::configure(h.cgroup_path, cfg.mem_limit, cfg.cpu_weight);
    h.timings.cgroup_ms = ms_since(cg_start);

    // ── Phase 2: Create pipes ─────────────────────────────
    int ready_pipe[2], sync_pipe[2];
    if (pipe2(ready_pipe, O_CLOEXEC) != 0)
        throw std::runtime_error("pipe2(ready) failed: " +
                                 std::string(strerror(errno)));
    if (pipe2(sync_pipe, O_CLOEXEC) != 0) {
        close(ready_pipe[0]); close(ready_pipe[1]);
        throw std::runtime_error("pipe2(sync) failed: " +
                                 std::string(strerror(errno)));
    }

    int go_pipe[2] = {-1, -1};
    if (cfg.parked) {
        if (pipe2(go_pipe, O_CLOEXEC) != 0) {
            close(ready_pipe[0]); close(ready_pipe[1]);
            close(sync_pipe[0]); close(sync_pipe[1]);
            throw std::runtime_error("pipe2(go) failed");
        }
    }

    // ── Phase 3: Prepare child args ───────────────────────
    ChildArgs args;
    args.rootfs_path  = cfg.rootfs_path;
    args.workload_bin = cfg.workload_bin;
    args.weights_path = cfg.weights_path;
    args.weights_size = cfg.weights_size;
    args.prefault     = cfg.prefault;
    args.hugepages    = cfg.hugepages;
    args.ready_wr     = ready_pipe[1];
    args.ready_rd     = ready_pipe[0];
    args.sync_rd      = sync_pipe[0];
    args.sync_wr      = sync_pipe[1];
    args.go_rd        = go_pipe[0];
    args.go_wr        = go_pipe[1];

    // ── Phase 4: Clone with new namespaces ────────────────
    auto ns_start = Clock::now();
    constexpr size_t kStackSize = 1024 * 1024;  // 1 MiB
    auto* stack = static_cast<char*>(malloc(kStackSize));
    if (!stack) throw std::runtime_error("malloc child stack failed");

    const int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
                            CLONE_NEWIPC | CLONE_NEWNET | SIGCHLD;
    pid_t pid = clone(child_fn, stack + kStackSize, clone_flags, &args);
    if (pid < 0) {
        int err = errno;
        free(stack);
        close(ready_pipe[0]); close(ready_pipe[1]);
        close(sync_pipe[0]); close(sync_pipe[1]);
        if (go_pipe[0] >= 0) { close(go_pipe[0]); close(go_pipe[1]); }
        throw std::runtime_error("clone failed: " + std::string(strerror(err)));
    }
    h.timings.ns_create_ms = ms_since(ns_start);

    // ── Phase 5: Add child to cgroup, then release it ─────
    cgroup::add_pid(h.cgroup_path, pid);

    char sync_byte = 'S';
    if (write(sync_pipe[1], &sync_byte, 1) != 1) {
        dprintf(STDERR_FILENO, "[launch] sync write failed\n");
    }
    close(sync_pipe[0]);
    close(sync_pipe[1]);

    // Close child-side fds in parent.
    close(ready_pipe[1]);  // parent doesn't write to ready pipe
    if (go_pipe[0] >= 0) close(go_pipe[0]);  // parent doesn't read go pipe

    h.pid = pid;
    h.ready_fd = ready_pipe[0];
    h.go_fd = (cfg.parked && go_pipe[1] >= 0) ? go_pipe[1] : -1;

    free(stack);
    h.timings.total_ms = ms_since(t0);
    return h;
}

double wait_ready(const SandboxHandle& h, int timeout_ms) {
    if (h.ready_fd < 0) return 0.0;

    auto start = Clock::now();

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
        throw std::runtime_error(
            "unexpected ready signal from workload (n=" + std::to_string(n) +
            " buf=" + std::to_string(static_cast<int>(buf)) + ")");
    }

    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void send_go(SandboxHandle& h) {
    if (h.go_fd >= 0) {
        char g = 'G';
        (void)write(h.go_fd, &g, 1);
        close(h.go_fd);
        h.go_fd = -1;
    }
}

void teardown(SandboxHandle& h) {
    if (h.go_fd >= 0) {
        close(h.go_fd);
        h.go_fd = -1;
    }
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
}

}  // namespace ws
