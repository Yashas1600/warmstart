// pool.cpp — warm-pool: pre-initialized sandboxes handed out on demand.

#include "pool.h"
#include "cgroup.h"

#include <stdexcept>

namespace ws {

WarmPool::WarmPool(const SandboxConfig& cfg, int pool_size)
    : cfg_(cfg), target_size_(pool_size) {
    // Pre-fill the pool.
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
    running_ = false;
    cv_.notify_all();

    if (refill_thread_.joinable()) {
        refill_thread_.join();
    }

    // Destroy remaining sandboxes.
    std::lock_guard lock(mu_);
    for (auto& h : pool_) {
        teardown(h);
    }
    pool_.clear();
}

void WarmPool::refill_loop() {
    while (running_) {
        std::unique_lock lock(mu_);

        // Wait until pool is below target or we're shutting down.
        cv_.wait(lock, [this] {
            return static_cast<int>(pool_.size()) < target_size_ || !running_;
        });

        if (!running_) break;

        // Refill one at a time, releasing the lock during creation.
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
    // Create a sandbox with namespaces, cgroup, and mounts ready,
    // but the workload paused (waiting for a signal to exec).
    // For the warm-pool strategy we use launch_cold() but the workload
    // is designed to wait for a trigger before doing heavy work.
    return launch_cold(cfg_);
}

}  // namespace ws
