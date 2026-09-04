#!/usr/bin/env python3
"""
plot_results.py — Visualize warmstart benchmark results.

Reads the CSV output from the benchmark harness and produces:
  1. Latency distribution (box + strip plot) per strategy
  2. Phase breakdown stacked bar chart
  3. CDF of startup latency per strategy
  4. Page fault comparison

Usage:
    python3 scripts/plot_results.py results.csv [output_dir]
"""

import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib
import numpy as np

matplotlib.use("Agg")  # non-interactive backend

def load_data(csv_path: str) -> pd.DataFrame:
    df = pd.read_csv(csv_path)
    return df

def plot_latency_distribution(df: pd.DataFrame, out_dir: str):
    """Box plot + scatter of total latency per strategy."""
    fig, ax = plt.subplots(figsize=(10, 6))
    strategies = df["strategy"].unique()

    data = [df[df["strategy"] == s]["total_ms"].values for s in strategies]
    bp = ax.boxplot(data, labels=strategies, patch_artist=True,
                    boxprops=dict(facecolor="#4C72B0", alpha=0.7))

    # Overlay individual points with jitter.
    for i, s in enumerate(strategies):
        y = df[df["strategy"] == s]["total_ms"].values
        x = np.random.normal(i + 1, 0.04, size=len(y))
        ax.scatter(x, y, alpha=0.3, s=10, color="#DD8452", zorder=3)

    ax.set_ylabel("Startup Latency (ms)")
    ax.set_title("Startup Latency Distribution by Strategy")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "latency_distribution.png"), dpi=150)
    plt.close(fig)
    print(f"  -> latency_distribution.png")

def plot_phase_breakdown(df: pd.DataFrame, out_dir: str):
    """Stacked bar chart: mean time per phase for each strategy."""
    phases = ["ns_create_ms", "cgroup_ms", "mount_ms", "exec_ms", "weight_load_ms"]
    labels = ["Namespace", "Cgroup", "Mount", "Exec", "Weights"]
    colors = ["#4C72B0", "#DD8452", "#55A868", "#C44E52", "#8172B3"]

    strategies = df["strategy"].unique()
    means = {s: [df[df["strategy"] == s][p].mean() for p in phases] for s in strategies}

    fig, ax = plt.subplots(figsize=(10, 6))
    x = np.arange(len(strategies))
    width = 0.5
    bottom = np.zeros(len(strategies))

    for i, (phase, label) in enumerate(zip(phases, labels)):
        vals = [means[s][i] for s in strategies]
        ax.bar(x, vals, width, bottom=bottom, label=label, color=colors[i])
        bottom += vals

    ax.set_xticks(x)
    ax.set_xticklabels(strategies)
    ax.set_ylabel("Mean Time (ms)")
    ax.set_title("Startup Phase Breakdown by Strategy")
    ax.legend(loc="upper right")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "phase_breakdown.png"), dpi=150)
    plt.close(fig)
    print(f"  -> phase_breakdown.png")

def plot_cdf(df: pd.DataFrame, out_dir: str):
    """CDF of startup latency per strategy."""
    fig, ax = plt.subplots(figsize=(10, 6))
    colors = ["#4C72B0", "#DD8452", "#55A868", "#C44E52"]

    for i, s in enumerate(df["strategy"].unique()):
        data = np.sort(df[df["strategy"] == s]["total_ms"].values)
        cdf = np.arange(1, len(data) + 1) / len(data)
        ax.plot(data, cdf, label=s, color=colors[i % len(colors)], linewidth=2)

    ax.set_xlabel("Startup Latency (ms)")
    ax.set_ylabel("CDF")
    ax.set_title("Cumulative Distribution of Startup Latency")
    ax.legend()
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "latency_cdf.png"), dpi=150)
    plt.close(fig)
    print(f"  -> latency_cdf.png")

def plot_page_faults(df: pd.DataFrame, out_dir: str):
    """Bar chart of mean page faults per strategy."""
    fig, ax = plt.subplots(figsize=(10, 6))
    strategies = df["strategy"].unique()

    minor = [df[df["strategy"] == s]["minor_faults"].mean() for s in strategies]
    major = [df[df["strategy"] == s]["major_faults"].mean() for s in strategies]

    x = np.arange(len(strategies))
    width = 0.35
    ax.bar(x - width/2, minor, width, label="Minor Faults", color="#4C72B0")
    ax.bar(x + width/2, major, width, label="Major Faults", color="#C44E52")

    ax.set_xticks(x)
    ax.set_xticklabels(strategies)
    ax.set_ylabel("Mean Page Faults")
    ax.set_title("Page Faults by Strategy")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "page_faults.png"), dpi=150)
    plt.close(fig)
    print(f"  -> page_faults.png")

def print_summary_table(df: pd.DataFrame):
    """Print p50/p90/p99 summary table."""
    print("\n" + "=" * 70)
    print("  BENCHMARK SUMMARY")
    print("=" * 70)
    print(f"{'Strategy':<14} {'p50 (ms)':>10} {'p90 (ms)':>10} {'p99 (ms)':>10} "
          f"{'mean':>10} {'stddev':>10} {'runs':>6}")
    print("-" * 70)

    for s in df["strategy"].unique():
        data = df[df["strategy"] == s]["total_ms"]
        p50 = data.quantile(0.50)
        p90 = data.quantile(0.90)
        p99 = data.quantile(0.99)
        mean = data.mean()
        std = data.std()
        n = len(data)
        print(f"{s:<14} {p50:>10.2f} {p90:>10.2f} {p99:>10.2f} "
              f"{mean:>10.2f} {std:>10.2f} {n:>6}")
    print("=" * 70)

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 plot_results.py <results.csv> [output_dir]")
        sys.exit(1)

    csv_path = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "."

    os.makedirs(out_dir, exist_ok=True)
    df = load_data(csv_path)

    print(f"Loaded {len(df)} results from {csv_path}")
    print_summary_table(df)

    print("\nGenerating plots...")
    plot_latency_distribution(df, out_dir)
    plot_phase_breakdown(df, out_dir)
    plot_cdf(df, out_dir)
    plot_page_faults(df, out_dir)
    print("\nDone!")

if __name__ == "__main__":
    main()
