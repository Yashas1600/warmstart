// zygote.cpp — template-process strategy with COW fork.
// The template is a real sandbox (clone + namespaces + cgroup + pivot_root)
// that pre-loads model weights. Children fork() from it and do per-request
// work on COW'd pages, timing the full fork-to-ready cycle.

#include "zygote.h"
#include "cgroup.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;

namespace ws {

namespace {

int pivot_root(const char* new_root, const char* put_old) {
    return syscall(SYS_pivot_root, new_root, put_old);
}

// Arguments for the zygote template clone child.
struct ZygoteTemplateArgs {
    std::string rootfs_path;
    std::string weights_path;
    uint64_t    weights_size;
    bool        prefault;
    bool        hugepages;

    int cmd_rd;      // read commands from parent ('F' = fork, 'Q' = quit)
    int cmd_wr;      // close in child
    int resp_rd;     // close in child
    int resp_wr;     // write responses to parent ('D' = done)
    int sync_rd;     // block until cgroup is set up
    int sync_wr;     // close in child
};

// Template process entry point (runs inside clone'd namespace).
int zygote_template_fn(void* arg) {
    auto* a = static_cast<ZygoteTemplateArgs*>(arg);

    // Close parent-side fds.
    close(a->cmd_wr);
    close(a->resp_rd);
    close(a->sync_wr);

    // Wait for cgroup setup.
    char sb;
    if (read(a->sync_rd, &sb, 1) != 1) _exit(110);
    close(a->sync_rd);

    // ── Pre-load weights (before pivot_root, host paths visible) ──
    void* weights = MAP_FAILED;
    size_t wsize = a->weights_size;

    if (!a->weights_path.empty()) {
        int wfd = open(a->weights_path.c_str(), O_RDONLY);
        if (wfd >= 0) {
            int mflags = MAP_PRIVATE;
            if (a->prefault) mflags |= MAP_POPULATE;
            weights = mmap(nullptr, wsize, PROT_READ, mflags, wfd, 0);
            close(wfd);
        }
    }
    if (weights == MAP_FAILED) {
        // Fallback: anonymous mmap.
        int mflags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (a->prefault) mflags |= MAP_POPULATE;
        weights = mmap(nullptr, wsize, PROT_READ | PROT_WRITE, mflags, -1, 0);
    }

    if (weights != MAP_FAILED) {
        if (a->hugepages)
            madvise(weights, wsize, MADV_HUGEPAGE);
        if (a->prefault)
            madvise(weights, wsize, MADV_WILLNEED);

        // Always pre-touch all pages so they're resident for children.
        volatile char sum = 0;
        auto* p = static_cast<const char*>(weights);
        for (size_t off = 0; off < wsize; off += 4096) {
            sum += p[off];
        }
    }

    // ── Mount setup + pivot_root (same path as cold sandbox) ──
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        dprintf(STDERR_FILENO, "[zygote-tmpl] MS_PRIVATE failed: %s\n",
                strerror(errno));
        _exit(100);
    }

    const char* rootfs = a->rootfs_path.c_str();
    if (mount(rootfs, rootfs, nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        dprintf(STDERR_FILENO, "[zygote-tmpl] rootfs bind failed: %s\n",
                strerror(errno));
        _exit(101);
    }

    std::string put_old = a->rootfs_path + "/.old_root";
    mkdir(put_old.c_str(), 0700);
    if (pivot_root(rootfs, put_old.c_str()) != 0) {
        dprintf(STDERR_FILENO, "[zygote-tmpl] pivot_root failed: %s\n",
                strerror(errno));
        _exit(102);
    }
    if (chdir("/") != 0) _exit(103);
    umount2("/.old_root", MNT_DETACH);
    rmdir("/.old_root");

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, nullptr);
    mkdir("/dev", 0755);
    mount("tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "size=64k");

    sethostname("warmstart-zygote", 16);

    // Signal parent that template is ready.
    char ready = 'T';
    (void)write(a->resp_wr, &ready, 1);

    // ── Command loop ──────────────────────────────────────
    int cmd_rd  = a->cmd_rd;
    int resp_wr = a->resp_wr;

    while (true) {
        char cmd;
        ssize_t n = read(cmd_rd, &cmd, 1);
        if (n <= 0 || cmd == 'Q') break;

        if (cmd == 'F') {
            // Create a per-fork ready pipe.
            int rp[2];
            if (pipe(rp) != 0) {
                dprintf(STDERR_FILENO, "[zygote-tmpl] pipe failed: %s\n",
                        strerror(errno));
                continue;
            }

            pid_t child = fork();
            if (child < 0) {
                close(rp[0]); close(rp[1]);
                dprintf(STDERR_FILENO, "[zygote-tmpl] fork failed: %s\n",
                        strerror(errno));
                continue;
            }

            if (child == 0) {
                // ── Zygote child ──────────────────────────
                close(rp[0]);
                close(cmd_rd);
                close(resp_wr);

                // Per-request work: touch all weight pages (COW read).
                // This is the same work the cold workload does after mmap.
                if (weights != MAP_FAILED) {
                    volatile char csum = 0;
                    auto* p = static_cast<const char*>(weights);
                    for (size_t off = 0; off < wsize; off += 4096) {
                        csum += p[off];
                    }
                }

                // Signal ready.
                char r = 'R';
                (void)write(rp[1], &r, 1);
                close(rp[1]);
                _exit(0);
            }

            // ── Template: wait for child, reap, report ────
            close(rp[1]);
            char child_ready;
            (void)read(rp[0], &child_ready, 1);
            close(rp[0]);
            waitpid(child, nullptr, 0);  // reap immediately

            char done = 'D';
            (void)write(resp_wr, &done, 1);
        }
    }

    if (weights != MAP_FAILED) munmap(weights, wsize);
    close(cmd_rd);
    close(resp_wr);
    _exit(0);
}

}  // namespace

Zygote::Zygote(const SandboxConfig& cfg) : cfg_(cfg) {}

Zygote::~Zygote() {
    shutdown();
}

void Zygote::init() {
    int cmd_pipe[2], resp_pipe[2], sync_pipe[2];
    if (pipe2(cmd_pipe, O_CLOEXEC) != 0 ||
        pipe2(resp_pipe, O_CLOEXEC) != 0 ||
        pipe2(sync_pipe, O_CLOEXEC) != 0) {
        throw std::runtime_error("zygote: pipe creation failed");
    }

    // Create cgroup for the template.
    cgroup_path_ = cgroup::create("zygote-template");
    cgroup::configure(cgroup_path_, cfg_.mem_limit, cfg_.cpu_weight);

    // Prepare args.
    ZygoteTemplateArgs args;
    args.rootfs_path  = cfg_.rootfs_path;
    args.weights_path = cfg_.weights_path;
    args.weights_size = cfg_.weights_size;
    args.prefault     = cfg_.prefault;
    args.hugepages    = cfg_.hugepages;
    args.cmd_rd       = cmd_pipe[0];
    args.cmd_wr       = cmd_pipe[1];
    args.resp_rd      = resp_pipe[0];
    args.resp_wr      = resp_pipe[1];
    args.sync_rd      = sync_pipe[0];
    args.sync_wr      = sync_pipe[1];

    // Clone with full namespace isolation (same as cold sandbox).
    constexpr size_t kStackSize = 1024 * 1024;
    auto* stack = static_cast<char*>(malloc(kStackSize));
    if (!stack) throw std::runtime_error("zygote: malloc stack failed");

    const int clone_flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
                            CLONE_NEWIPC | CLONE_NEWNET | SIGCHLD;
    pid_t pid = clone(zygote_template_fn, stack + kStackSize,
                      clone_flags, &args);
    if (pid < 0) {
        int err = errno;
        free(stack);
        close(cmd_pipe[0]); close(cmd_pipe[1]);
        close(resp_pipe[0]); close(resp_pipe[1]);
        close(sync_pipe[0]); close(sync_pipe[1]);
        throw std::runtime_error("zygote: clone failed: " +
                                 std::string(strerror(err)));
    }

    // Add template to cgroup, then release it.
    cgroup::add_pid(cgroup_path_, pid);
    char sb = 'S';
    (void)write(sync_pipe[1], &sb, 1);
    close(sync_pipe[0]);
    close(sync_pipe[1]);

    // Close child-side fds in parent.
    close(cmd_pipe[0]);
    close(resp_pipe[1]);

    cmd_fd_       = cmd_pipe[1];
    resp_fd_      = resp_pipe[0];
    template_pid_ = pid;
    free(stack);

    // Wait for template to signal it's ready (weights loaded, pivot done).
    char tmpl_ready;
    if (read(resp_fd_, &tmpl_ready, 1) != 1 || tmpl_ready != 'T') {
        shutdown();
        throw std::runtime_error("zygote: template did not signal ready");
    }
}

SandboxHandle Zygote::fork_child() {
    SandboxHandle h;
    if (template_pid_ <= 0) {
        throw std::runtime_error("zygote: template not initialized");
    }

    auto t0 = Clock::now();

    // Tell template to fork a child.
    char cmd = 'F';
    if (write(cmd_fd_, &cmd, 1) != 1) {
        throw std::runtime_error("zygote: failed to send fork command");
    }

    // Wait for template to report the child completed (fork + work + ready).
    char done;
    ssize_t n = read(resp_fd_, &done, 1);
    if (n != 1 || done != 'D') {
        throw std::runtime_error("zygote: failed to receive done signal");
    }

    auto t1 = Clock::now();
    double total = std::chrono::duration<double, std::milli>(t1 - t0).count();

    h.timings.total_ms = total;
    // Child already exited and was reaped by the template.
    h.pid = -1;
    h.ready_fd = -1;

    return h;
}

void Zygote::shutdown() {
    if (cmd_fd_ >= 0) {
        char cmd = 'Q';
        (void)write(cmd_fd_, &cmd, 1);
        close(cmd_fd_);
        cmd_fd_ = -1;
    }
    if (resp_fd_ >= 0) {
        close(resp_fd_);
        resp_fd_ = -1;
    }
    if (template_pid_ > 0) {
        // Template should exit on 'Q'. Wait with timeout, then SIGKILL.
        int status;
        pid_t w = waitpid(template_pid_, &status, WNOHANG);
        if (w == 0) {
            // Not exited yet — give it a moment, then kill.
            usleep(50000);
            w = waitpid(template_pid_, &status, WNOHANG);
            if (w == 0) {
                kill(template_pid_, SIGKILL);
                waitpid(template_pid_, nullptr, 0);
            }
        }
        template_pid_ = -1;
    }
    if (!cgroup_path_.empty()) {
        cgroup::destroy(cgroup_path_);
        cgroup_path_.clear();
    }
}

}  // namespace ws
