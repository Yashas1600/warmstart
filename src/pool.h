#pragma once
// pool.h — warm-pool strategy: keep N pre-initialized sandboxes ready.
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

    // Grab a pre-initialized sandbox. Blocks if pool is empty.
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
    int next_id_{0};
};

}  // namespace ws
