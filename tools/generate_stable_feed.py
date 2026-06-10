#!/usr/bin/env python3
"""Stable mid with occasional 1-tick jitter, wide spread.
   The MM should print reliably because the market doesn't trend away
   after each fill."""
import csv, random, sys
from pathlib import Path

def generate(path, n=2000, seed=1, start_mid=10000, spread=10, size=100):
    rng = random.Random(seed)
    mid = start_mid
    ts = 0
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ts_ns","symbol","bid_px","bid_sz","ask_px","ask_sz"])
        for _ in range(n):
            # Mostly sits; occasional 1-tick jitter that can trade through
            # resting quotes on either side.
            mid += rng.choice([-1, 0, 0, 0, 0, 1])
            bid = mid - spread // 2
            ask = bid + spread
            w.writerow([ts, "TEST", bid, size, ask, size])
            ts += 1_000_000

if __name__ == "__main__":
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "data/stable_feed.csv")
    generate(out)
    print(f"wrote {out}")
