#pragma once
// pool.h — warm-pool strategy: keep N pre-initialized sandboxes ready.
// Parked sandboxes have namespaces, cgroups, and mounts set up but block
// before exec. acquire() returns one; the caller sends go + waits for ready.
// A background thread refills the pool after one is handed out.

#include "sandbox.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace ws {

class WarmPool {
public:
    explicit WarmPool(const SandboxConfig& cfg, int pool_size = 4);
    ~WarmPool();

    // Grab a pre-initialized (parked) sandbox. Blocks if pool is empty.
    // The caller must call send_go(h) then wait_ready(h) to start the workload.
    SandboxHandle acquire();

    // Current number of ready sandboxes.
    int available() const;

    // Shut down the refill thread and destroy remaining sandboxes.
    void shutdown();

private:
    void refill_loop();
    SandboxHandle create_parked_sandbox();

    SandboxConfig cfg_;
    int target_size_;
    std::atomic<bool> running_{true};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<SandboxHandle> pool_;
    std::thread refill_thread_;
};

}  // namespace ws
