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
3. Child waits on sync pipe until parent confirms cgroup membership (no race)
4. `mount(/, MS_REC|MS_PRIVATE)` for systemd compatibility, bind-mount rootfs, `pivot_root`
5. Mount `/proc` and `/dev`, set hostname
6. `dup2` ready pipe to fd 3, `exec()` the workload binary
7. Workload mmaps weights file, touches all pages, writes `'R'` to fd 3

This is the baseline — every phase contributes to latency.

### Warm Pool (`--strategy warm-pool`)
Pre-initialize N sandboxes so they're ready before a request arrives:
1. At startup, create N sandboxes with parked=true (namespaces, cgroups, mounts all ready, child blocks on a go pipe before exec)
2. A background thread refills the pool after each handout
3. On request, hand out a parked sandbox, send go signal — child proceeds to exec the workload
4. Timed portion is go → workload ready (only exec + weight loading on the hot path)

### Zygote (`--strategy zygote`)
A template process pre-loads expensive state; children inherit it via COW `fork()`:
1. Template is created via the same `clone()` + namespace + cgroup + `pivot_root` path as cold start
2. Template mmaps the weights file and touches all pages (pre-loading into page cache)
3. On request, template calls `fork()` — child gets all mapped memory via copy-on-write
4. Child touches all weight pages (COW read — pages already resident, so fast), signals ready
5. Template waits for child ready, reaps it, reports to parent. Full fork-to-ready timing is measured.

**Namespace sharing tradeoff:** Zygote children inherit the template's namespaces via `fork()`. They share PID, mount, UTS, IPC, and network namespaces with the template and with each other. This is a deliberate tradeoff — `fork()` is fast (<1ms overhead) precisely because it doesn't create new namespaces, only COW page table entries. For production use with multi-tenant isolation requirements, the warm-pool strategy provides per-request namespace isolation at the cost of higher startup latency.

## Memory Optimizations

Toggle with `--prefault` and `--hugepages`:

| Flag | Effect |
|------|--------|
| `--prefault` | Uses `MAP_POPULATE` on mmap + `madvise(MADV_WILLNEED)` to prefault all pages upfront |
| `--hugepages` | `madvise(MADV_HUGEPAGE)` to request transparent huge pages, reducing TLB misses |

## Benchmark Results

**Machine:** Docker Desktop Linux VM (linuxkit 6.10.14), 10 cores (Apple Silicon), 16 GB RAM.
**Weights:** 512 MB file, 100 runs per strategy.

| Strategy | p50 (ms) | p90 (ms) | p99 (ms) | mean (ms) | stddev |
|----------|----------|----------|----------|-----------|--------|
| cold | 9.04 | 20.02 | 44.18 | 12.14 | 8.65 |
| warm-pool | 7.41 | 9.78 | 19.68 | 8.25 | 2.57 |
| zygote | 8.36 | 9.01 | 11.83 | 8.71 | 3.32 |

Cold start p50 = 9ms with high variance (stddev 8.65) due to cgroup setup cost on first run (~27ms) and occasional page cache eviction spikes up to ~100ms. Warm-pool eliminates namespace/cgroup/mount overhead and achieves p50 = 7.4ms with much lower variance. Zygote achieves similar p50 but the tightest p99 (11.83ms) because COW page reads from pre-loaded weights hit the page cache consistently.

## Building

```bash
# Requirements: Linux, CMake 3.20+, C++20 compiler (GCC 11+ or Clang 14+)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run tests (need root + cgroups v2 + prepared rootfs)
cd build && ctest --output-on-failure
```

## Quick Start

```bash
# 1. Build a minimal rootfs
sudo bash scripts/build_rootfs.sh /tmp/warmstart-rootfs

# 2. Copy the workload into it (statically linked)
sudo cp build/dummy_workload /tmp/warmstart-rootfs/workload

# 3. Create a dummy weights file (512 MB)
bash scripts/create_weights.sh 512 /tmp/warmstart-weights.bin

# 4. Run cold start
sudo ./build/warmstart --strategy cold \
    --rootfs /tmp/warmstart-rootfs \
    --weights /tmp/warmstart-weights.bin \
    --weights-size 536870912

# 5. Run full benchmark
sudo ./build/warmstart --strategy benchmark \
    --runs 100 --csv results.csv \
    --rootfs /tmp/warmstart-rootfs \
    --weights /tmp/warmstart-weights.bin \
    --weights-size 536870912

# 6. Plot results
python3 scripts/plot_results.py results.csv plots/
```

## Running on macOS

warmstart requires Linux kernel APIs (namespaces, cgroups). On a Mac, use Docker:

```bash
docker build -t warmstart -f docker/Dockerfile .
docker run --privileged --rm -it warmstart --strategy benchmark --runs 100
```

Or use a Linux VM via [OrbStack](https://orbstack.dev/), [Lima](https://github.com/lima-vm/lima), or UTM.

## What I Learned

### Where startup time actually goes

Profiling 100 cold starts with 512 MB weights on a 10-core VM:

1. **Cgroup setup is the hidden first-run cost.** The first cold start takes ~27ms just for cgroup creation because `ensure_root_delegation` moves all root cgroup processes to a holding group and enables cpu+memory controllers. Subsequent cgroup creates take <0.1ms. This is a one-time cost that dominates the first request.

2. **Weight loading is the dominant per-request cost (~7-10ms for 512 MB).** Touching every 4K page of a 512 MB file triggers ~131K minor faults when pages are in the page cache, or major faults on first access. After the first cold-start run populates the page cache, subsequent runs are faster.

3. **Namespace creation is consistent (~0.7-1ms).** The `clone()` syscall with five namespace flags takes under 1ms reliably. This is a small fraction of total latency.

4. **Variance is the real enemy.** Cold-start p99 (44ms) is 5x the p50 (9ms). The spikes come from page cache pressure — when the kernel evicts weight pages to make room for new sandbox allocations, the workload hits major faults and slows dramatically.

### Key insights

- **Warm-pool wins on variance, not just mean.** The pool pre-creates namespaces and cgroups during refill (off the hot path), so the only per-request work is exec + weight loading. The stddev drops from 8.65ms (cold) to 2.57ms (warm-pool).

- **Zygote has the tightest tail latency.** The p99 of 11.83ms vs cold's 44.18ms is the standout result. Because the template pre-loads all weight pages and children read them via COW (no new allocations, no page cache misses), zygote is immune to the page eviction spikes that plague cold start.

- **COW reads are not free but are predictable.** Zygote children still touch all 131K pages, which takes ~8ms. The time comes from page table walks and TLB misses, not from I/O. This is CPU-bound work with low variance, unlike the I/O-bound page faults in cold start.

- **The cgroup race matters.** Without the sync pipe ensuring the child is in its cgroup before doing work, early child activity (mount syscalls, mmap) would escape cgroup accounting and potentially exceed memory limits.

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
│   ├── test_pool.cpp         # Pool logic unit tests
│   └── test_integration.cpp  # End-to-end strategy + zombie tests
├── scripts/
│   ├── build_rootfs.sh       # Build Alpine/busybox rootfs
│   ├── create_weights.sh     # Generate dummy weights file
│   └── plot_results.py       # Matplotlib visualization
├── docker/
│   ├── Dockerfile            # Build + run in Linux container
│   └── entrypoint.sh         # Container setup script
├── results.csv               # Raw benchmark data (300 runs)
└── plots/                    # Generated latency/phase/CDF charts
```

## License

MIT
