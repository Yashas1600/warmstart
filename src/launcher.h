#pragma once
// launcher.h — top-level API that ties strategies and benchmarking together.

#include "benchmark.h"
#include "sandbox.h"
#include <string>

namespace ws {

struct LauncherOpts {
    std::string strategy = "cold";  // cold | warm-pool | zygote | benchmark
    std::string rootfs;
    std::string workload;
    std::string weights_path;
    uint64_t    weights_size = 1ULL << 30;
    uint64_t    mem_limit    = 2ULL << 30;
    int         cpu_weight   = 100;
    int         pool_size    = 4;
    bool        prefault     = false;
    bool        hugepages    = false;
    int         bench_runs   = 100;
    std::string csv_output   = "results.csv";
};

// Parse CLI args into LauncherOpts.
LauncherOpts parse_args(int argc, char* argv[]);

// Run the chosen strategy or benchmark.
int run(const LauncherOpts& opts);

}  // namespace ws
