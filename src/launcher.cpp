// launcher.cpp — CLI parsing and strategy dispatch.

#include "launcher.h"
#include "pool.h"
#include "zygote.h"

#include <cstring>
#include <iostream>

namespace ws {

namespace {

void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --strategy <cold|warm-pool|zygote|benchmark>\n"
        << "                          Startup strategy (default: cold)\n"
        << "  --rootfs <path>         Path to rootfs directory\n"
        << "  --workload <path>       Path to workload binary inside rootfs\n"
        << "  --weights <path>        Path to weights file on host\n"
        << "  --weights-size <bytes>  Size of weights file (default: 1GB)\n"
        << "  --mem-limit <bytes>     Cgroup memory limit (default: 2GB)\n"
        << "  --cpu-weight <int>      Cgroup cpu.weight (default: 100)\n"
        << "  --pool-size <int>       Warm-pool size (default: 4)\n"
        << "  --prefault              Enable MAP_POPULATE / MADV_WILLNEED\n"
        << "  --hugepages             Enable MADV_HUGEPAGE\n"
        << "  --runs <int>            Benchmark iterations (default: 100)\n"
        << "  --csv <path>            CSV output path (default: results.csv)\n"
        << "  --help                  Show this message\n";
}

}  // namespace

LauncherOpts parse_args(int argc, char* argv[]) {
    LauncherOpts opts;

    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* name) {
            return std::strcmp(argv[i], name) == 0;
        };
        auto next = [&]() -> const char* {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Missing value for " << argv[i] << "\n";
            std::exit(1);
        };

        if (arg("--strategy"))      opts.strategy     = next();
        else if (arg("--rootfs"))   opts.rootfs       = next();
        else if (arg("--workload")) opts.workload     = next();
        else if (arg("--weights"))  opts.weights_path = next();
        else if (arg("--weights-size")) opts.weights_size = std::stoull(next());
        else if (arg("--mem-limit"))    opts.mem_limit    = std::stoull(next());
        else if (arg("--cpu-weight"))   opts.cpu_weight   = std::stoi(next());
        else if (arg("--pool-size"))    opts.pool_size    = std::stoi(next());
        else if (arg("--prefault"))     opts.prefault     = true;
        else if (arg("--hugepages"))    opts.hugepages    = true;
        else if (arg("--runs"))         opts.bench_runs   = std::stoi(next());
        else if (arg("--csv"))          opts.csv_output   = next();
        else if (arg("--help")) { print_usage(argv[0]); std::exit(0); }
        else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }

    return opts;
}

int run(const LauncherOpts& opts) {
    SandboxConfig scfg;
    scfg.rootfs_path  = opts.rootfs.empty() ? "/tmp/warmstart-rootfs" : opts.rootfs;
    scfg.workload_bin = opts.workload.empty() ? "/workload" : opts.workload;
    scfg.weights_path = opts.weights_path;
    scfg.weights_size = opts.weights_size;
    scfg.mem_limit    = opts.mem_limit;
    scfg.cpu_weight   = opts.cpu_weight;
    scfg.prefault     = opts.prefault;
    scfg.hugepages    = opts.hugepages;

    if (opts.strategy == "cold") {
        std::cout << "[warmstart] Launching cold sandbox...\n";
        auto h = launch_cold(scfg);
        std::cout << "[warmstart] Sandbox PID: " << h.pid << "\n";
        std::cout << "[warmstart] Phase timings:\n"
                  << "  namespace: " << h.timings.ns_create_ms << " ms\n"
                  << "  cgroup:    " << h.timings.cgroup_ms    << " ms\n"
                  << "  mount:     " << h.timings.mount_ms     << " ms\n"
                  << "  total:     " << h.timings.total_ms     << " ms\n";
        try {
            double wait = wait_ready(h);
            std::cout << "[warmstart] Workload ready in " << wait << " ms\n";
        } catch (const std::exception& e) {
            std::cerr << "[warmstart] " << e.what() << "\n";
        }
        teardown(h);

    } else if (opts.strategy == "warm-pool") {
        std::cout << "[warmstart] Starting warm pool (size="
                  << opts.pool_size << ")...\n";
        WarmPool pool(scfg, opts.pool_size);
        auto h = pool.acquire();
        std::cout << "[warmstart] Acquired sandbox PID: " << h.pid << "\n";
        try {
            double wait = wait_ready(h);
            std::cout << "[warmstart] Workload ready in " << wait << " ms\n";
        } catch (const std::exception& e) {
            std::cerr << "[warmstart] " << e.what() << "\n";
        }
        teardown(h);
        pool.shutdown();

    } else if (opts.strategy == "zygote") {
        std::cout << "[warmstart] Initializing zygote template...\n";
        Zygote zyg(scfg);
        zyg.init();
        std::cout << "[warmstart] Forking child from template...\n";
        auto h = zyg.fork_child();
        std::cout << "[warmstart] Child PID: " << h.pid
                  << " (fork latency: " << h.timings.total_ms << " ms)\n";
        teardown(h);
        zyg.shutdown();

    } else if (opts.strategy == "benchmark") {
        BenchmarkConfig bcfg;
        bcfg.num_runs   = opts.bench_runs;
        bcfg.prefault   = opts.prefault;
        bcfg.hugepages  = opts.hugepages;
        bcfg.pool_size  = opts.pool_size;
        bcfg.output_csv = opts.csv_output;

        auto results = run_benchmarks(scfg, bcfg);
        write_csv(results, bcfg.output_csv);
        print_summary(results);
        std::cout << "\nResults written to " << bcfg.output_csv << "\n";

    } else {
        std::cerr << "Unknown strategy: " << opts.strategy << "\n";
        return 1;
    }

    return 0;
}

}  // namespace ws
