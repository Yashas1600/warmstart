#!/usr/bin/env bash
# entrypoint.sh — prepare rootfs, create weights file, run warmstart.
set -euo pipefail

echo "=== warmstart container ==="
echo "Kernel: $(uname -r)"
echo "CPU: $(grep -c ^processor /proc/cpuinfo) cores"
echo "RAM: $(awk '/MemTotal/ {printf "%.0f MB", $2/1024}' /proc/meminfo)"

# Build rootfs if it doesn't exist.
if [ ! -d /tmp/warmstart-rootfs ]; then
    echo "[setup] Building rootfs..."
    bash /warmstart/scripts/build_rootfs.sh /tmp/warmstart-rootfs
fi

# Copy workload binary into rootfs.
cp /usr/local/bin/dummy_workload /tmp/warmstart-rootfs/workload
chmod +x /tmp/warmstart-rootfs/workload

# Create dummy weights file (512 MB default).
WEIGHTS_SIZE_MB="${WS_WEIGHTS_MB:-512}"
WEIGHTS_FILE="/tmp/warmstart-weights.bin"
if [ ! -f "${WEIGHTS_FILE}" ]; then
    echo "[setup] Creating ${WEIGHTS_SIZE_MB} MB weights file..."
    dd if=/dev/urandom of="${WEIGHTS_FILE}" bs=1M count="${WEIGHTS_SIZE_MB}" status=progress 2>&1
fi

WEIGHTS_SIZE_BYTES=$((WEIGHTS_SIZE_MB * 1024 * 1024))

echo "[setup] Ready. Running warmstart..."
exec warmstart \
    --rootfs /tmp/warmstart-rootfs \
    --workload /workload \
    --weights "${WEIGHTS_FILE}" \
    --weights-size "${WEIGHTS_SIZE_BYTES}" \
    --mem-limit $((WEIGHTS_SIZE_BYTES * 3)) \
    "$@"
