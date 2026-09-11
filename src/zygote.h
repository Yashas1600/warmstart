#pragma once
// zygote.h — zygote/template-process strategy.
// A template process is created via clone() with full namespace isolation,
// pivot_root, and pre-loaded weights (mmapped + touched). Children fork()
// from the template and get COW memory — pages are already resident.
//
// NOTE: zygote children share the template's namespaces (PID, mount, UTS,
// IPC, network). They are NOT per-request isolated. See README for the
// tradeoff discussion.

#include "sandbox.h"
#include <sys/types.h>
#include <string>

namespace ws {

class Zygote {
public:
    explicit Zygote(const SandboxConfig& cfg);
    ~Zygote();

    // Create the template process: clone with namespaces, pivot_root,
    // mmap weights, touch all pages. Blocks until template is ready.
    void init();

    // Fork a child from the template that does per-request work (touch
    // all weight pages), signals ready, then exits. Blocks until child
    // is done. Returns a handle with the total fork-to-ready timing.
    SandboxHandle fork_child();

    // Tear down the template process.
    void shutdown();

    bool is_ready() const { return template_pid_ > 0; }

private:
    SandboxConfig cfg_;
    pid_t template_pid_ = -1;
    int   cmd_fd_       = -1;   // write-end: send 'F' or 'Q' to template
    int   resp_fd_      = -1;   // read-end: receive 'D' (done) from template
    std::string cgroup_path_;
};

}  // namespace ws
