#!/usr/bin/env bash
# entrypoint.sh — prepare rootfs, create weights file, run warmstart.
set -euo pipefail

echo "=== warmstart container ==="

# Build rootfs if it doesn't exist.
if [ ! -d /tmp/warmstart-rootfs ]; then
    bash /warmstart/scripts/build_rootfs.sh /tmp/warmstart-rootfs
    cp /usr/local/bin/dummy_workload /tmp/warmstart-rootfs/workload
    chmod +x /tmp/warmstart-rootfs/workload
fi

# Create dummy weights file (256 MB for quick testing in container).
if [ ! -f /tmp/warmstart-weights.bin ]; then
    dd if=/dev/urandom of=/tmp/warmstart-weights.bin bs=1M count=256 status=progress
fi

exec warmstart \
    --rootfs /tmp/warmstart-rootfs \
    --workload /workload \
    --weights /tmp/warmstart-weights.bin \
    --weights-size $((256 * 1024 * 1024)) \
    "$@"
