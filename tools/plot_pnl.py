#!/usr/bin/env python3
"""
Plot the PnL/inventory time series written by run_backtest.

Usage:
    python3 tools/plot_pnl.py [pnl.csv] [--out pnl.png] [--title TEXT]
                              [--tick-size 0.25] [--tick-value 12.5]

Three stacked panels: mid, net position, PnL. With --tick-size the mid is shown
in price, and with --tick-value the PnL is shown in dollars. Timestamps that are
real epoch nanoseconds are shown as UTC clock time. Needs matplotlib only.
"""
import argparse
import csv
import sys
from datetime import datetime, timezone
from pathlib import Path


def load(path):
    ts, pos, real, unreal, mid = [], [], [], [], []
    with Path(path).open() as f:
        for row in csv.DictReader(f):
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
    ap.add_argument("--out", default=None, help="output image; if omitted, show on screen")
    ap.add_argument("--title", default=None)
    ap.add_argument("--tick-size", type=float, default=None, help="price per tick, e.g. 0.25 for ES")
    ap.add_argument("--tick-value", type=float, default=None, help="dollars per tick per contract")
    args = ap.parse_args()

    try:
        import matplotlib.dates as mdates
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib not installed. pip install matplotlib")

    ts, pos, real, unreal, mid = load(args.path)
    clock = ts and ts[0] > 1e17  # real epoch nanoseconds, not a replay counter
    t = ([datetime.fromtimestamp(x / 1e9, tz=timezone.utc) for x in ts] if clock
         else [x / 1e9 for x in ts])

    px = args.tick_size or 1.0
    usd = args.tick_value or 1.0
    pnl = [(r + u) * usd for r, u in zip(real, unreal)]
    realized = [r * usd for r in real]
    tm = [t[i] for i, m in enumerate(mid) if m is not None]
    mm = [m * px for m in mid if m is not None]

    fig, axes = plt.subplots(3, 1, sharex=True, figsize=(10, 7))
    for ax in axes:
        ax.spines[["top", "right"]].set_visible(False)
        ax.grid(alpha=0.3)

    axes[0].plot(tm, mm, lw=0.8, color="#2166ac")
    axes[0].set_ylabel("mid (price)" if args.tick_size else "mid (ticks)")
    axes[0].set_title(args.title or args.path)

    axes[1].step(t, pos, where="post", lw=0.8, color="#2166ac")
    axes[1].axhline(0, color="#333333", lw=0.5)
    axes[1].set_ylabel("net position")

    axes[2].plot(t, pnl, lw=0.8, color="#2166ac", label="total")
    axes[2].plot(t, realized, lw=0.8, color="#8c8c8c", label="realized")
    axes[2].axhline(0, color="#333333", lw=0.5)
    axes[2].set_ylabel("PnL (US$)" if args.tick_value else "PnL (ticks)")
    axes[2].legend(loc="upper left", frameon=False)

    if clock:
        axes[2].xaxis.set_major_formatter(mdates.DateFormatter("%H:%M", tz=timezone.utc))
        axes[2].set_xlabel("time (UTC)")
    else:
        axes[2].set_xlabel("time (s)")

    fig.tight_layout()
    if args.out:
        fig.savefig(args.out, dpi=150)
        print(f"wrote {args.out}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
