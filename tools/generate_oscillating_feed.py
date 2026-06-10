#!/usr/bin/env python3
"""
Oscillating mid: the price wobbles symmetrically around a center, with
occasional larger excursions. This is the friendliest feed for a market
maker — they can capture the bid-ask spread on every reversal.
"""
import csv, math, random, sys
from pathlib import Path

def generate(path, n=3000, seed=7, center=10000, amplitude=15, period=200,
             spread=8, size=100, step_ns=1_000_000):
    rng = random.Random(seed)
    ts = 0
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ts_ns","symbol","bid_px","bid_sz","ask_px","ask_sz"])
        for i in range(n):
            phase = 2 * math.pi * i / period
            # Sine wave + small noise.
            mid = center + int(round(amplitude * math.sin(phase)
                                     + rng.gauss(0, 1.0)))
            bid = mid - spread // 2
            ask = bid + spread
            w.writerow([ts, "TEST", bid, size, ask, size])
            ts += step_ns

if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "data/oscillating_feed.csv")
    generate(out)
    print(f"wrote {out}")
