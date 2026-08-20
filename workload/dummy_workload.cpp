// dummy_workload.cpp — simulates an AI inference workload.
// Mmaps a configurable-size file (model weights), touches pages,
// then signals readiness by writing 'R' to fd 3 (or stdout).

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    // Read config from environment (set by the launcher).
    const char* size_env    = std::getenv("WS_WEIGHTS_SIZE");
    const char* path_env    = std::getenv("WS_WEIGHTS_PATH");
    const char* prefault_env = std::getenv("WS_PREFAULT");
    const char* huge_env    = std::getenv("WS_HUGEPAGES");

    size_t weights_size = size_env ? std::stoull(size_env) : (1ULL << 30);
    bool   prefault     = prefault_env && prefault_env[0] == '1';
    bool   hugepages    = huge_env && huge_env[0] == '1';

    auto t0 = std::chrono::steady_clock::now();

    void* mapping = MAP_FAILED;

    if (path_env && path_env[0]) {
        // Mmap an existing file (real weights simulation).
        int fd = open(path_env, O_RDONLY);
        if (fd < 0) {
            std::cerr << "[workload] Cannot open weights: "
                      << std::strerror(errno) << "\n";
            return 1;
        }

        int flags = MAP_PRIVATE;
        if (prefault) flags |= MAP_POPULATE;

        mapping = mmap(nullptr, weights_size, PROT_READ, flags, fd, 0);
        close(fd);
    } else {
        // Anonymous mapping as fallback (no real file).
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (prefault) flags |= MAP_POPULATE;
        mapping = mmap(nullptr, weights_size, PROT_READ | PROT_WRITE,
                        flags, -1, 0);
    }

    if (mapping == MAP_FAILED) {
        std::cerr << "[workload] mmap failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    // Apply memory advice.
    if (prefault) {
        madvise(mapping, weights_size, MADV_WILLNEED);
    }
    if (hugepages) {
        madvise(mapping, weights_size, MADV_HUGEPAGE);
    }

    // Touch pages to simulate weight loading / tensor initialization.
    // Read first byte of every 4K page.
    volatile char checksum = 0;
    const auto* p = static_cast<const char*>(mapping);
    for (size_t off = 0; off < weights_size; off += 4096) {
        checksum += p[off];
    }

    auto t1 = std::chrono::steady_clock::now();
    double load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cerr << "[workload] Weights loaded: " << (weights_size >> 20)
              << " MB in " << load_ms << " ms"
              << " (prefault=" << prefault << " hugepages=" << hugepages << ")\n";

    // Signal ready: write 'R' to fd 3 (the pipe from launcher),
    // falling back to stdout if fd 3 isn't open.
    char ready = 'R';
    if (write(3, &ready, 1) < 0) {
        write(STDOUT_FILENO, &ready, 1);
    }

    // Keep running until killed (simulates serving).
    pause();

    munmap(mapping, weights_size);
    return 0;
}
