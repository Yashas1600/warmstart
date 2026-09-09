// pool.cpp — warm-pool: pre-initialized sandboxes handed out on demand.

#include "pool.h"

#include <stdexcept>

namespace ws {

WarmPool::WarmPool(const SandboxConfig& cfg, int pool_size)
    : cfg_(cfg), target_size_(pool_size) {
    // Pre-fill the pool with parked sandboxes.
    for (int i = 0; i < target_size_; ++i) {
        pool_.push_back(create_parked_sandbox());
    }
    // Start background refill thread.
    refill_thread_ = std::thread(&WarmPool::refill_loop, this);
}

WarmPool::~WarmPool() {
    shutdown();
}

SandboxHandle WarmPool::acquire() {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [this] { return !pool_.empty() || !running_; });

    if (!running_ && pool_.empty()) {
        throw std::runtime_error("pool shut down with no sandboxes available");
    }

    auto h = std::move(pool_.front());
    pool_.pop_front();

    // Notify refill thread that we need a replacement.
    cv_.notify_all();
    return h;
}

int WarmPool::available() const {
    std::lock_guard lock(mu_);
    return static_cast<int>(pool_.size());
}

void WarmPool::shutdown() {
    bool was_running = running_.exchange(false);
    if (!was_running) return;  // already shut down

    cv_.notify_all();
    if (refill_thread_.joinable()) {
        refill_thread_.join();
    }

    // Destroy remaining parked sandboxes.
    std::lock_guard lock(mu_);
    for (auto& h : pool_) {
        teardown(h);
    }
    pool_.clear();
}

void WarmPool::refill_loop() {
    while (running_) {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [this] {
            return static_cast<int>(pool_.size()) < target_size_ || !running_;
        });

        if (!running_) break;

        while (static_cast<int>(pool_.size()) < target_size_ && running_) {
            lock.unlock();
            auto h = create_parked_sandbox();
            lock.lock();
            if (running_) {
                pool_.push_back(std::move(h));
                cv_.notify_all();
            } else {
                teardown(h);
            }
        }
    }
}

SandboxHandle WarmPool::create_parked_sandbox() {
    // Create a sandbox with cfg_.parked=true: the clone child sets up
    // namespaces, cgroup, and mounts, then blocks on the go pipe
    // before exec'ing the workload.
    auto parked_cfg = cfg_;
    parked_cfg.parked = true;
    return launch_cold(parked_cfg);
}

}  // namespace ws
