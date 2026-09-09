// dummy_workload.cpp — simulates an AI inference workload.
// Mmaps a configurable-size file (model weights), touches every page,
// then signals readiness by writing 'R' to fd 3 (the pipe from the launcher).

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    const char* size_env     = std::getenv("WS_WEIGHTS_SIZE");
    const char* path_env     = std::getenv("WS_WEIGHTS_PATH");
    const char* prefault_env = std::getenv("WS_PREFAULT");
    const char* huge_env     = std::getenv("WS_HUGEPAGES");

    size_t weights_size = size_env ? std::stoull(size_env) : (1ULL << 30);
    bool   prefault     = prefault_env && prefault_env[0] == '1';
    bool   hugepages    = huge_env && huge_env[0] == '1';

    auto t0 = std::chrono::steady_clock::now();

    void* mapping = MAP_FAILED;

    if (path_env && path_env[0]) {
        // Mmap the weights file (bind-mounted into rootfs by launcher).
        int fd = open(path_env, O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "[workload] cannot open weights %s: %s\n",
                    path_env, strerror(errno));
            return 1;
        }
        int flags = MAP_PRIVATE;
        if (prefault) flags |= MAP_POPULATE;
        mapping = mmap(nullptr, weights_size, PROT_READ, flags, fd, 0);
        close(fd);
    } else {
        // Anonymous mapping fallback.
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
        if (prefault) flags |= MAP_POPULATE;
        mapping = mmap(nullptr, weights_size, PROT_READ | PROT_WRITE,
                       flags, -1, 0);
    }

    if (mapping == MAP_FAILED) {
        dprintf(STDERR_FILENO, "[workload] mmap failed: %s\n", strerror(errno));
        return 1;
    }

    if (prefault)  madvise(mapping, weights_size, MADV_WILLNEED);
    if (hugepages) madvise(mapping, weights_size, MADV_HUGEPAGE);

    // Touch every 4K page to simulate weight loading / tensor init.
    volatile char checksum = 0;
    const auto* p = static_cast<const char*>(mapping);
    for (size_t off = 0; off < weights_size; off += 4096) {
        checksum += p[off];
    }

    auto t1 = std::chrono::steady_clock::now();
    double load_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    dprintf(STDERR_FILENO, "[workload] weights loaded: %zu MB in %.1f ms "
            "(prefault=%d hugepages=%d)\n",
            weights_size >> 20, load_ms, prefault, hugepages);

    // Signal ready: write 'R' to fd 3 (the pipe set up by the launcher).
    // If fd 3 is not open, exit with error — do not fall back to stdout.
    char ready = 'R';
    if (write(3, &ready, 1) != 1) {
        dprintf(STDERR_FILENO, "[workload] failed to write ready signal to fd 3: %s\n",
                strerror(errno));
        munmap(mapping, weights_size);
        return 1;
    }

    // Keep running until killed (simulates serving).
    pause();

    munmap(mapping, weights_size);
    return 0;
}
