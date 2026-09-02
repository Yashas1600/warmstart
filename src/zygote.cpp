// zygote.cpp — template-process strategy with COW fork.

#include "zygote.h"
#include "cgroup.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#include <sys/mman.h>
#include <fcntl.h>
#endif

namespace ws {

Zygote::Zygote(const SandboxConfig& cfg) : cfg_(cfg) {}

Zygote::~Zygote() {
    shutdown();
}

void Zygote::init() {
#ifdef __linux__
    // Create command pipe (parent -> template) and response pipe (template -> parent).
    int cmd_pipe[2], resp_pipe[2];
    if (pipe2(cmd_pipe, O_CLOEXEC) != 0 || pipe2(resp_pipe, O_CLOEXEC) != 0) {
        throw std::runtime_error("zygote: pipe creation failed");
    }

    // Create cgroup for the template.
    cgroup_path_ = cgroup::create("zygote-template");
    cgroup::configure(cgroup_path_, cfg_.mem_limit, cfg_.cpu_weight);

    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("zygote: fork failed: " +
                                 std::string(std::strerror(errno)));
    }

    if (pid == 0) {
        // ── Template process ──────────────────────────────
        close(cmd_pipe[1]);   // close write end
        close(resp_pipe[0]);  // close read end
        int cmd_rd  = cmd_pipe[0];
        int resp_wr = resp_pipe[1];

        // Pre-load the weights file via mmap.
        void* weights = MAP_FAILED;
        if (!cfg_.weights_path.empty()) {
            int wfd = open(cfg_.weights_path.c_str(), O_RDONLY);
            if (wfd >= 0) {
                int flags = MAP_PRIVATE;
                if (cfg_.prefault) flags |= MAP_POPULATE;
                weights = mmap(nullptr, cfg_.weights_size, PROT_READ,
                               flags, wfd, 0);
                close(wfd);

                if (weights != MAP_FAILED) {
                    if (cfg_.prefault) {
                        madvise(weights, cfg_.weights_size, MADV_WILLNEED);
                    }
                    if (cfg_.hugepages) {
                        madvise(weights, cfg_.weights_size, MADV_HUGEPAGE);
                    }
                }
            }
        }

        // Sit in a loop: wait for 'F' command, fork, report child pid.
        while (true) {
            char cmd;
            ssize_t n = read(cmd_rd, &cmd, 1);
            if (n <= 0 || cmd == 'Q') break;  // quit

            if (cmd == 'F') {
                pid_t child = fork();
                if (child == 0) {
                    // Child of zygote: weights are already mapped (COW).
                    // Touch a page to simulate workload initialization.
                    if (weights != MAP_FAILED) {
                        volatile char sum = 0;
                        auto* p = static_cast<const char*>(weights);
                        // Touch first byte of each 4K page in first 1MB.
                        size_t touch_size = std::min(cfg_.weights_size,
                                                     (uint64_t)(1 << 20));
                        for (size_t off = 0; off < touch_size; off += 4096) {
                            sum += p[off];
                        }
                    }
                    // Signal ready: write 'R' to stdout (fd 1).
                    // In practice, the workload binary handles this.
                    _exit(0);
                }
                // Report child pid back to parent.
                pid_t cpid = child;
                write(resp_wr, &cpid, sizeof(cpid));
            }
        }

        if (weights != MAP_FAILED) {
            munmap(weights, cfg_.weights_size);
        }
        _exit(0);
    }

    // ── Parent ────────────────────────────────────────────
    close(cmd_pipe[0]);
    close(resp_pipe[1]);
    cmd_fd_  = cmd_pipe[1];
    resp_fd_ = resp_pipe[0];
    template_pid_ = pid;

    cgroup::add_pid(cgroup_path_, pid);
#else
    // Non-Linux stub.
    template_pid_ = 1;  // fake
#endif
}

SandboxHandle Zygote::fork_child() {
    SandboxHandle h;

#ifdef __linux__
    if (template_pid_ <= 0) {
        throw std::runtime_error("zygote: template not initialized");
    }

    auto t0 = std::chrono::steady_clock::now();

    // Send fork command.
    char cmd = 'F';
    if (write(cmd_fd_, &cmd, 1) != 1) {
        throw std::runtime_error("zygote: failed to send fork command");
    }

    // Read child pid.
    pid_t child_pid;
    if (read(resp_fd_, &child_pid, sizeof(child_pid)) != sizeof(child_pid)) {
        throw std::runtime_error("zygote: failed to read child pid");
    }

    auto t1 = std::chrono::steady_clock::now();
    double fork_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    h.pid = child_pid;
    h.timings.total_ms = fork_ms;
    h.timings.ns_create_ms = 0;    // inherited
    h.timings.cgroup_ms    = 0;    // inherited
    h.timings.mount_ms     = 0;    // inherited
    h.timings.exec_ms      = fork_ms;
    h.timings.weight_load_ms = 0;  // COW — no reload
#endif

    return h;
}

void Zygote::shutdown() {
#ifdef __linux__
    if (cmd_fd_ >= 0) {
        char cmd = 'Q';
        write(cmd_fd_, &cmd, 1);
        close(cmd_fd_);
        cmd_fd_ = -1;
    }
    if (resp_fd_ >= 0) {
        close(resp_fd_);
        resp_fd_ = -1;
    }
    if (template_pid_ > 0) {
        waitpid(template_pid_, nullptr, 0);
        template_pid_ = -1;
    }
    if (!cgroup_path_.empty()) {
        cgroup::destroy(cgroup_path_);
        cgroup_path_.clear();
    }
#endif
}

}  // namespace ws
