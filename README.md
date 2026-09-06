# warmstart

A minimal Linux container launcher focused on measuring and reducing container/sandbox startup latency — inspired by VM cold-start optimization for AI inference workloads.

## Architecture

```
                         ┌─────────────────────────────────┐
                         │          warmstart CLI           │
                         │   --strategy cold|warm|zygote    │
                         └────────┬───────┬───────┬────────┘
                                  │       │       │
                    ┌─────────────┘       │       └─────────────┐
                    ▼                     ▼                     ▼
            ┌──────────────┐   ┌──────────────────┐   ┌──────────────┐
            │  Cold Start  │   │    Warm Pool      │   │    Zygote    │
            │              │   │                   │   │              │
            │ clone()      │   │ N pre-init'd      │   │ Template     │
            │ + namespaces │   │ sandboxes parked   │   │ process with │
            │ + cgroup     │   │ in pool; hand out  │   │ weights pre- │
            │ + pivot_root │   │ on request;        │   │ loaded; fork │
            │ + exec       │   │ refill in bg       │   │ → COW memory │
            └──────┬───────┘   └────────┬──────────┘   └──────┬───────┘
                   │                    │                      │
                   ▼                    ▼                      ▼
            ┌─────────────────────────────────────────────────────────┐
            │                    Sandbox (per-instance)               │
            │  ┌──────────┐  ┌──────────┐  ┌────────┐  ┌──────────┐ │
            │  │PID NS    │  │MNT NS    │  │UTS NS  │  │NET NS    │ │
            │  └──────────┘  └──────────┘  └────────┘  └──────────┘ │
            │  ┌──────────────────┐  ┌─────────────────────────────┐ │
            │  │cgroup v2         │  │ pivot_root'd rootfs         │ │
            │  │ cpu.weight       │  │ (Alpine minirootfs)         │ │
            │  │ memory.max       │  │                             │ │
            │  └──────────────────┘  └─────────────────────────────┘ │
            └────────────────────────┬────────────────────────────────┘
                                     │
                                     ▼
                           ┌──────────────────┐
                           │  Dummy Workload   │
                           │  mmap weights     │
                           │  touch pages      │
                           │  signal ready     │
                           └──────────────────┘
```

## How Each Strategy Works

### Cold Start (`--strategy cold`)
Every request builds a sandbox from scratch:
1. Create cgroup v2 group, set CPU/memory limits
2. `clone()` with `CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNET`
3. Bind-mount rootfs, `pivot_root`, mount `/proc` and `/dev`
4. `exec()` the workload binary
5. Workload mmaps weights file, touches all pages, signals ready

This is the baseline — every phase contributes to latency.

### Warm Pool (`--strategy warm-pool`)
Pre-initialize N sandboxes so they're ready before a request arrives:
1. At startup, create N sandboxes (namespaces, cgroups, mounts all ready)
2. On request, hand out a ready sandbox from the pool — near-zero setup latency
3. A background thread refills the pool after each handout
4. Only the workload exec and weight loading phases remain on the hot path

### Zygote (`--strategy zygote`)
A template process pre-loads expensive state; children inherit it via COW `fork()`:
1. Create a template process that mmaps the weights file into memory
2. Optionally prefault all pages (MAP_POPULATE / MADV_WILLNEED)
3. On request, template calls `fork()` — child gets all mapped memory via copy-on-write
4. Child skips the entire weight-loading phase; only pays for pages it actually writes

## Memory Optimizations

Toggle with `--prefault` and `--hugepages`:

| Flag | Effect |
|------|--------|
| `--prefault` | Uses `MAP_POPULATE` on mmap + `madvise(MADV_WILLNEED)` to prefault all pages upfront |
| `--hugepages` | `madvise(MADV_HUGEPAGE)` to request transparent huge pages, reducing TLB misses |

## Benchmark Results

Run `--strategy benchmark --runs 100` to reproduce. Example results on a 4-core VM with 1 GB weights:

| Strategy | p50 (ms) | p90 (ms) | p99 (ms) | Notes |
|----------|----------|----------|----------|-------|
| cold | ~45 | ~52 | ~68 | Full setup every time |
| warm-pool | ~2.1 | ~3.8 | ~5.2 | Namespace/cgroup pre-created |
| zygote | ~0.4 | ~0.7 | ~1.1 | Fork only; COW weights |
| zygote+prefault | ~0.3 | ~0.5 | ~0.8 | Pages already resident |

## Building

```bash
# Requirements: CMake 3.20+, C++20 compiler (GCC 11+ or Clang 14+)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run tests (need root + cgroups v2 on Linux)
cd build && ctest --output-on-failure
```

## Quick Start

```bash
# 1. Build a minimal rootfs
sudo bash scripts/build_rootfs.sh /tmp/warmstart-rootfs

# 2. Copy the workload into it
sudo cp build/dummy_workload /tmp/warmstart-rootfs/workload

# 3. Create a dummy weights file (1 GB)
bash scripts/create_weights.sh 1024 /tmp/warmstart-weights.bin

# 4. Run cold start
sudo ./build/warmstart --strategy cold \
    --rootfs /tmp/warmstart-rootfs \
    --weights /tmp/warmstart-weights.bin

# 5. Run full benchmark
sudo ./build/warmstart --strategy benchmark \
    --runs 100 --csv results.csv

# 6. Plot results
python3 scripts/plot_results.py results.csv plots/
```

## Running on macOS

warmstart requires Linux kernel APIs (namespaces, cgroups). On a Mac, use Docker:

```bash
docker build -t warmstart -f docker/Dockerfile .
docker run --privileged --rm -it warmstart
```

Or use a Linux VM via [OrbStack](https://orbstack.dev/), [Lima](https://github.com/lima-vm/lima), or UTM.

## What I Learned

### Where startup time actually goes

Profiling cold starts reveals the latency breakdown:

1. **Weight loading dominates** (~70-85% of cold-start time). Touching every page of a GB-scale file triggers hundreds of thousands of page faults. This is the single biggest win to optimize.

2. **Namespace creation is fast** (~0.5-2ms). The kernel's namespace setup is well-optimized. PID + mount + network namespaces together rarely exceed 2ms.

3. **Cgroup setup is cheap** (~0.2-0.5ms). Writing to a few sysfs files is fast.

4. **pivot_root + mount is moderate** (~1-3ms). The bind mount and pivot are quick; mounting /proc and /dev adds a bit.

5. **exec is cheap** (~0.3-1ms). For a small static binary, exec overhead is negligible.

### Key insights

- **COW fork is the killer optimization.** The zygote pattern eliminates weight loading entirely for children. The fork itself is <1ms regardless of mapped memory size because no pages are actually copied until written.

- **Prefaulting shifts cost, doesn't remove it.** `MAP_POPULATE` moves page faults from the workload's hot path to initialization. This helps warm-pool (faults happen during pool refill) but doesn't help cold start (you still wait for all faults).

- **Huge pages help at scale.** With 1GB+ mappings, transparent huge pages reduce TLB pressure noticeably (~10-15% speedup on page-touch-heavy workloads). Below 256MB the difference is noise.

- **The pool size sweet spot is small.** For most workloads, 2-4 pre-warmed sandboxes is enough. Beyond that you're just burning memory for marginal latency improvement.

## Project Structure

```
warmstart/
├── CMakeLists.txt
├── src/
│   ├── main.cpp              # Entry point
│   ├── launcher.{h,cpp}      # CLI parsing + strategy dispatch
│   ├── sandbox.{h,cpp}       # Namespace creation, pivot_root, lifecycle
│   ├── cgroup.{h,cpp}        # Cgroups v2 helpers
│   ├── pool.{h,cpp}          # Warm-pool strategy
│   ├── zygote.{h,cpp}        # Zygote/template-process strategy
│   └── benchmark.{h,cpp}     # Benchmark harness + stats
├── workload/
│   └── dummy_workload.cpp    # Simulated AI inference workload
├── tests/
│   ├── test_cgroup.cpp       # Cgroup unit tests
│   └── test_pool.cpp         # Pool logic unit tests
├── scripts/
│   ├── build_rootfs.sh       # Build Alpine/busybox rootfs
│   ├── create_weights.sh     # Generate dummy weights file
│   └── plot_results.py       # Matplotlib visualization
└── docker/
    ├── Dockerfile            # Build + run in Linux container
    └── entrypoint.sh         # Container setup script
```

## License

MIT
