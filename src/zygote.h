#pragma once
// zygote.h — zygote/template-process strategy.
// A template process pre-loads heavy state (mmap'd weights file).
// Children fork() from it and get COW memory — no reload needed.

#include "sandbox.h"
#include <sys/types.h>

namespace ws {

class Zygote {
public:
    explicit Zygote(const SandboxConfig& cfg);
    ~Zygote();

    // Initialize the template process: create sandbox, mmap weights, pause.
    void init();

    // Fork a child from the template. Returns a handle to the new process.
    SandboxHandle fork_child();

    // Tear down the template process.
    void shutdown();

    bool is_ready() const { return template_pid_ > 0; }

private:
    SandboxConfig cfg_;
    pid_t template_pid_ = -1;
    int   cmd_fd_       = -1;   // write-end: send 'F' to request fork
    int   resp_fd_      = -1;   // read-end:  receive child pid
    std::string cgroup_path_;
};

}  // namespace ws
