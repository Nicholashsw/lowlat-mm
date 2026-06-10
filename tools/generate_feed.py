#!/usr/bin/env python3
"""
Generate a synthetic top-of-book feed for the backtester.

Output columns: ts_ns, symbol, bid_px, bid_sz, ask_px, ask_sz
All prices are already in integer tick units.

The process is a lightly mean-reverting random walk on the mid with a
constant 2-tick spread and 100-lot top-of-book size. Deterministic with a
fixed seed so the committed sample is reproducible.
"""
import csv
import random
import sys
from pathlib import Path

def generate(path: Path, n: int = 5000, seed: int = 42, start_mid: int = 10000,
             spread: int = 2, size: int = 100, symbol: str = "TEST",
             step_ns: int = 1_000_000):
    rng = random.Random(seed)
    mid = start_mid
    ts = 0
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ts_ns", "symbol", "bid_px", "bid_sz", "ask_px", "ask_sz"])
        for _ in range(n):
            # Mean-revert toward start_mid with small drift kicks.
            mr = 0.01 * (start_mid - mid)
            drift = rng.gauss(mr, 1.0)
            mid += int(round(drift))
            bid = mid - spread // 2
            ask = bid + spread
            w.writerow([ts, symbol, bid, size, ask, size])
            ts += step_ns

if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "data/sample_feed.csv")
    out.parent.mkdir(parents=True, exist_ok=True)
    generate(out)
    print(f"wrote {out}")
