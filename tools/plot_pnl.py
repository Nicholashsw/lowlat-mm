#!/usr/bin/env python3
"""
Plot the PnL/inventory time series produced by run_backtest.

Usage:
    python3 tools/plot_pnl.py [pnl.csv] [--out pnl.png]

Renders three stacked panels: mid, net position, total PnL. Uses
matplotlib only — no other dependencies.
"""
import argparse
import csv
import sys
from pathlib import Path

def load(path):
    ts, pos, real, unreal, mid = [], [], [], [], []
    with Path(path).open() as f:
        r = csv.DictReader(f)
        for row in r:
            ts.append(int(row["ts_ns"]))
            pos.append(int(row["net_position"]))
            real.append(float(row["realized_pnl_ticks"]))
            unreal.append(float(row["unrealized_pnl_ticks"]))
            m = row["mid_ticks"].strip()
            mid.append(float(m) if m else None)
    return ts, pos, real, unreal, mid

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", nargs="?", default="pnl.csv")
    ap.add_argument("--out", default=None,
                    help="output image path; if omitted, show interactively")
    args = ap.parse_args()

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib not installed. pip install matplotlib")

    ts, pos, real, unreal, mid = load(args.path)
    # ns -> seconds for readability
    t = [x / 1e9 for x in ts]
    pnl = [r + u for r, u in zip(real, unreal)]

    fig, axes = plt.subplots(3, 1, sharex=True, figsize=(10, 7))
    # Mid panel skips Nones (one-sided book)
    tm = [t[i] for i, m in enumerate(mid) if m is not None]
    mm = [m for m in mid if m is not None]
    axes[0].plot(tm, mm, lw=1.0)
    axes[0].set_ylabel("mid (ticks)")
    axes[0].set_title(args.path)

    axes[1].plot(t, pos, lw=1.0)
    axes[1].axhline(0, color="k", lw=0.5)
    axes[1].set_ylabel("net position")

    axes[2].plot(t, pnl, lw=1.0, label="total PnL")
    axes[2].plot(t, real, lw=0.7, alpha=0.7, label="realized")
    axes[2].axhline(0, color="k", lw=0.5)
    axes[2].set_ylabel("PnL (ticks)")
    axes[2].set_xlabel("time (s)")
    axes[2].legend(loc="best")

    fig.tight_layout()
    if args.out:
        fig.savefig(args.out, dpi=120)
        print(f"wrote {args.out}")
    else:
        plt.show()

if __name__ == "__main__":
    main()
