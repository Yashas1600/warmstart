# Dockerfile — build and test warmstart in a Linux container.
# Required because warmstart uses Linux-specific APIs (namespaces, cgroups).
#
# Build:
#   docker build -t warmstart -f docker/Dockerfile .
#
# Run (needs --privileged for namespace + cgroup access):
#   docker run --privileged --rm -it warmstart
#
# For Mac users: Docker Desktop provides a Linux VM, so this works as-is.
# Alternatively, use a dedicated Linux VM (e.g., via Lima, UTM, or OrbStack).

FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake g++ python3 python3-pip python3-venv \
    curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /warmstart
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build -j$(nproc)

# ── Runtime stage ──────────────────────────────────────────
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-pip python3-venv \
    curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install Python deps for plotting.
RUN python3 -m venv /opt/venv && \
    /opt/venv/bin/pip install pandas matplotlib numpy

WORKDIR /warmstart

COPY --from=builder /warmstart/build/warmstart /usr/local/bin/warmstart
COPY --from=builder /warmstart/build/dummy_workload /usr/local/bin/dummy_workload
COPY scripts/ scripts/

# Build rootfs on container start.
COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
CMD ["--strategy", "benchmark", "--runs", "100"]
