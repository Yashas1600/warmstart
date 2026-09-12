// benchmark.cpp — run each strategy N times, collect stats, report.

#include "benchmark.h"
#include "pool.h"
#include "zygote.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sys/resource.h>

namespace ws {

namespace {

using Clock = std::chrono::steady_clock;

long get_minor_faults() {
    struct rusage ru;
    getrusage(RUSAGE_CHILDREN, &ru);
    return ru.ru_minflt;
}

long get_major_faults() {
    struct rusage ru;
    getrusage(RUSAGE_CHILDREN, &ru);
    return ru.ru_majflt;
}

double percentile(std::vector<double>& data, double p) {
    if (data.empty()) return 0;
    std::sort(data.begin(), data.end());
    double idx = p / 100.0 * static_cast<double>(data.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = lo + 1;
    if (hi >= data.size()) return data.back();
    double frac = idx - static_cast<double>(lo);
    return data[lo] * (1 - frac) + data[hi] * frac;
}

RunResult run_single_cold(const SandboxConfig& cfg, int run_id) {
    long mf_before = get_minor_faults();
    long MF_before = get_major_faults();

    auto t0 = Clock::now();
    auto h = launch_cold(cfg);
    double wait_ms = wait_ready(h, 30000);
    auto t1 = Clock::now();

    long mf_after = get_minor_faults();
    long MF_after = get_major_faults();

    RunResult r;
    r.strategy       = "cold";
    r.run_id         = run_id;
    r.total_ms       = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.ns_create_ms   = h.timings.ns_create_ms;
    r.cgroup_ms      = h.timings.cgroup_ms;
    r.mount_ms       = 0;  // child-side, not separately measurable from parent
    r.exec_ms        = 0;
    r.weight_load_ms = 0;
    r.minor_faults   = mf_after - mf_before;
    r.major_faults   = MF_after - MF_before;
    r.rss_kb         = 0;

    teardown(h);
    return r;
}

RunResult run_single_warm(WarmPool& pool, int run_id) {
    // Acquire a parked sandbox (not timed — pool wait is not startup latency).
    auto h = pool.acquire();

    long mf_before = get_minor_faults();
    long MF_before = get_major_faults();

    // Timed portion: send go -> workload ready.
    auto t0 = Clock::now();
    send_go(h);
    double wait_ms = wait_ready(h, 30000);
    auto t1 = Clock::now();

    long mf_after = get_minor_faults();
    long MF_after = get_major_faults();

    RunResult r;
    r.strategy       = "warm-pool";
    r.run_id         = run_id;
    r.total_ms       = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.ns_create_ms   = 0;  // pre-created
    r.cgroup_ms      = 0;  // pre-created
    r.mount_ms       = 0;  // pre-created
    r.exec_ms        = 0;
    r.weight_load_ms = 0;
    r.minor_faults   = mf_after - mf_before;
    r.major_faults   = MF_after - MF_before;
    r.rss_kb         = 0;

    teardown(h);
    return r;
}

RunResult run_single_zygote(Zygote& zyg, int run_id) {
    long mf_before = get_minor_faults();
    long MF_before = get_major_faults();

    // fork_child blocks until the child finishes work and signals ready.
    auto t0 = Clock::now();
    auto h = zyg.fork_child();
    auto t1 = Clock::now();

    long mf_after = get_minor_faults();
    long MF_after = get_major_faults();

    RunResult r;
    r.strategy       = "zygote";
    r.run_id         = run_id;
    r.total_ms       = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.ns_create_ms   = 0;  // inherited from template
    r.cgroup_ms      = 0;
    r.mount_ms       = 0;
    r.exec_ms        = 0;
    r.weight_load_ms = 0;  // COW — pages already resident
    r.minor_faults   = mf_after - mf_before;
    r.major_faults   = MF_after - MF_before;
    r.rss_kb         = 0;

    // No teardown needed — child already exited and was reaped by template.
    return r;
}

}  // namespace

std::vector<RunResult> run_benchmarks(const SandboxConfig& sandbox_cfg,
                                      const BenchmarkConfig& bench_cfg) {
    std::vector<RunResult> results;
    results.reserve(bench_cfg.num_runs * 3);

    auto scfg = sandbox_cfg;
    scfg.prefault  = bench_cfg.prefault;
    scfg.hugepages = bench_cfg.hugepages;

    // ── Cold start ────────────────────────────────────────
    if (bench_cfg.run_cold) {
        std::cout << "[bench] Running " << bench_cfg.num_runs
                  << " cold-start iterations...\n";
        for (int i = 0; i < bench_cfg.num_runs; ++i) {
            results.push_back(run_single_cold(scfg, i));
            if ((i + 1) % 10 == 0) {
                std::cout << "  cold: " << (i + 1) << "/"
                          << bench_cfg.num_runs << "\n";
            }
        }
    }

    // ── Warm pool ─────────────────────────────────────────
    if (bench_cfg.run_warm) {
        std::cout << "[bench] Running " << bench_cfg.num_runs
                  << " warm-pool iterations (pool=" << bench_cfg.pool_size
                  << ")...\n";
        WarmPool pool(scfg, bench_cfg.pool_size);
        for (int i = 0; i < bench_cfg.num_runs; ++i) {
            results.push_back(run_single_warm(pool, i));
            if ((i + 1) % 10 == 0) {
                std::cout << "  warm: " << (i + 1) << "/"
                          << bench_cfg.num_runs << "\n";
            }
        }
        pool.shutdown();
    }

    // ── Zygote ────────────────────────────────────────────
    if (bench_cfg.run_zygote) {
        std::cout << "[bench] Running " << bench_cfg.num_runs
                  << " zygote iterations...\n";
        Zygote zyg(scfg);
        zyg.init();
        for (int i = 0; i < bench_cfg.num_runs; ++i) {
            results.push_back(run_single_zygote(zyg, i));
            if ((i + 1) % 10 == 0) {
                std::cout << "  zygote: " << (i + 1) << "/"
                          << bench_cfg.num_runs << "\n";
            }
        }
        zyg.shutdown();
    }

    return results;
}

void write_csv(const std::vector<RunResult>& results, const std::string& path) {
    std::ofstream f(path);
    f << "strategy,run_id,total_ms,ns_create_ms,cgroup_ms,mount_ms,"
         "exec_ms,weight_load_ms,minor_faults,major_faults,rss_kb\n";

    for (const auto& r : results) {
        f << r.strategy << ","
          << r.run_id << ","
          << std::fixed << std::setprecision(3)
          << r.total_ms << ","
          << r.ns_create_ms << ","
          << r.cgroup_ms << ","
          << r.mount_ms << ","
          << r.exec_ms << ","
          << r.weight_load_ms << ","
          << r.minor_faults << ","
          << r.major_faults << ","
          << r.rss_kb << "\n";
    }
}

void print_summary(const std::vector<RunResult>& results) {
    std::map<std::string, std::vector<double>> by_strategy;
    for (const auto& r : results) {
        by_strategy[r.strategy].push_back(r.total_ms);
    }

    std::cout << "\n"
              << std::string(70, '=') << "\n"
              << "  BENCHMARK SUMMARY\n"
              << std::string(70, '=') << "\n\n";

    std::cout << std::left << std::setw(14) << "Strategy"
              << std::right
              << std::setw(10) << "p50 (ms)"
              << std::setw(10) << "p90 (ms)"
              << std::setw(10) << "p99 (ms)"
              << std::setw(10) << "mean"
              << std::setw(10) << "stddev"
              << std::setw(8)  << "runs"
              << "\n"
              << std::string(70, '-') << "\n";

    for (auto& [name, latencies] : by_strategy) {
        double p50 = percentile(latencies, 50);
        double p90 = percentile(latencies, 90);
        double p99 = percentile(latencies, 99);
        double mean = std::accumulate(latencies.begin(), latencies.end(), 0.0)
                      / static_cast<double>(latencies.size());
        double var = 0;
        for (double v : latencies) var += (v - mean) * (v - mean);
        double stddev = std::sqrt(var / static_cast<double>(latencies.size()));

        std::cout << std::left << std::setw(14) << name
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(10) << p50
                  << std::setw(10) << p90
                  << std::setw(10) << p99
                  << std::setw(10) << mean
                  << std::setw(10) << stddev
                  << std::setw(8) << latencies.size()
                  << "\n";
    }

    std::cout << std::string(70, '=') << "\n";
}

}  // namespace ws
