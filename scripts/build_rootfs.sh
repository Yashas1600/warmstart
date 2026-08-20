#!/usr/bin/env bash
# build_rootfs.sh — create a minimal rootfs for warmstart sandboxes.
# Uses Alpine Linux minirootfs or busybox as base.
#
# Usage: sudo ./build_rootfs.sh [output_dir]
#
# The resulting rootfs contains just enough to run the dummy workload.

set -euo pipefail

ROOTFS="${1:-/tmp/warmstart-rootfs}"
ALPINE_VERSION="3.19"
ALPINE_ARCH="x86_64"
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION}/releases/${ALPINE_ARCH}/alpine-minirootfs-${ALPINE_VERSION}.0-${ALPINE_ARCH}.tar.gz"

echo "[rootfs] Building minimal rootfs at ${ROOTFS}"

mkdir -p "${ROOTFS}"

# ── Option 1: Alpine minirootfs (preferred) ────────────────
if command -v curl &>/dev/null; then
    echo "[rootfs] Downloading Alpine ${ALPINE_VERSION} minirootfs..."
    TARBALL="/tmp/alpine-minirootfs.tar.gz"

    if [ ! -f "${TARBALL}" ]; then
        curl -fSL "${ALPINE_URL}" -o "${TARBALL}"
    fi

    tar xzf "${TARBALL}" -C "${ROOTFS}"
    echo "[rootfs] Alpine minirootfs extracted."

# ── Option 2: Busybox static (fallback) ───────────────────
elif command -v busybox &>/dev/null; then
    echo "[rootfs] Using busybox to build rootfs..."
    mkdir -p "${ROOTFS}"/{bin,sbin,usr/bin,usr/sbin,proc,dev,tmp,etc}
    cp "$(which busybox)" "${ROOTFS}/bin/busybox"

    # Install busybox symlinks.
    chroot "${ROOTFS}" /bin/busybox --install -s
    echo "[rootfs] Busybox rootfs created."

else
    echo "[rootfs] ERROR: Need curl (for Alpine) or busybox."
    exit 1
fi

# ── Common setup ──────────────────────────────────────────
mkdir -p "${ROOTFS}"/{proc,dev,sys,tmp,etc}

# Minimal /etc/resolv.conf (not really needed since we use CLONE_NEWNET).
echo "nameserver 8.8.8.8" > "${ROOTFS}/etc/resolv.conf"

# Create a directory for the workload binary.
mkdir -p "${ROOTFS}/opt"

echo "[rootfs] Done. Copy your workload binary to ${ROOTFS}/workload"
echo "[rootfs] Example:"
echo "  cp build/dummy_workload ${ROOTFS}/workload"
