#pragma once
// benchmark.h — harness that runs each strategy N times and collects stats.

#include "sandbox.h"
#include <string>
#include <vector>

namespace ws {

struct RunResult {
    std::string strategy;
    int         run_id;
    double      total_ms;
    double      ns_create_ms;
    double      cgroup_ms;
    double      mount_ms;
    double      exec_ms;
    double      weight_load_ms;
    long        minor_faults;
    long        major_faults;
    long        rss_kb;
};

struct BenchmarkConfig {
    int  num_runs     = 100;
    bool run_cold     = true;
    bool run_warm     = true;
    bool run_zygote   = true;
    bool prefault     = false;
    bool hugepages    = false;
    int  pool_size    = 4;
    std::string output_csv = "results.csv";
};

// Run the full benchmark suite. Returns all individual run results.
std::vector<RunResult> run_benchmarks(const SandboxConfig& sandbox_cfg,
                                      const BenchmarkConfig& bench_cfg);

// Write results to CSV file.
void write_csv(const std::vector<RunResult>& results, const std::string& path);

// Print summary table (p50/p90/p99) to stdout.
void print_summary(const std::vector<RunResult>& results);

}  // namespace ws
