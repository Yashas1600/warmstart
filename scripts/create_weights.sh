#!/usr/bin/env bash
# create_weights.sh — generate a dummy weights file for benchmarking.
# Usage: ./create_weights.sh [size_mb] [output_path]

set -euo pipefail

SIZE_MB="${1:-1024}"
OUTPUT="${2:-/tmp/warmstart-weights.bin}"

echo "[weights] Creating ${SIZE_MB} MB dummy weights file at ${OUTPUT}..."
dd if=/dev/urandom of="${OUTPUT}" bs=1M count="${SIZE_MB}" status=progress 2>&1
echo "[weights] Done: $(du -h "${OUTPUT}" | cut -f1)"
