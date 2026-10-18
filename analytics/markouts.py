#!/usr/bin/env python3
"""
Fill markouts for a lowlat-mm blotter, in pandas: the same table as markouts.q.

    python3 analytics/markouts.py --feed data/es_feed.csv --fills trades.csv [--out markouts.csv]

The markout at horizon h is side * (prevailing mid at fill time + h - fill price),
in ticks. The 0 ms markout is the half spread captured; its decay to five minutes
is the adverse selection. mae_1m / mfe_1m are the worst and best mid over the
minute after the fill, counting the mid prevailing at the fill.
"""
import argparse

import numpy as np
import pandas as pd

HORIZONS = {"0ms": 0, "1ms": 1, "100ms": 100, "1s": 1_000, "10s": 10_000, "1m": 60_000, "5m": 300_000}


def markouts(feed, fills):
    q = feed.assign(mid=0.5 * (feed["bid_px"] + feed["ask_px"]))[["ts_ns", "mid"]].sort_values("ts_ns", kind="stable")
    # several quotes can share a timestamp; the last one is the prevailing mid
    q = q.drop_duplicates("ts_ns", keep="last")
    f = fills.rename(columns={"order_id": "id", "px_ticks": "px"}).copy()
    f["side"] = np.where(f["side"] == "B", 1, -1)
    t = q["ts_ns"].to_numpy()
    mid = q["mid"].to_numpy()

    def mid_at(ts):
        i = np.searchsorted(t, ts, side="right") - 1
        return np.where(i >= 0, mid[np.clip(i, 0, None)], np.nan)

    for name, ms in HORIZONS.items():
        f[f"mo_{name}"] = f["side"] * (mid_at(f["ts_ns"].to_numpy() + ms * 1_000_000) - f["px"])

    lo, hi = [], []
    for ts in f["ts_ns"].to_numpy():
        start = max(np.searchsorted(t, ts, side="right") - 1, 0)
        end = np.searchsorted(t, ts + 60_000_000_000, side="right")
        window = mid[start:end]
        lo.append(window.min() if len(window) else np.nan)
        hi.append(window.max() if len(window) else np.nan)
    lo, hi = np.array(lo), np.array(hi)
    f["mae_1m"] = np.where(f["side"] == 1, lo - f["px"], f["px"] - hi)
    f["mfe_1m"] = np.where(f["side"] == 1, hi - f["px"], f["px"] - lo)
    return f


def summary(f):
    cols = [f"mo_{h}" for h in HORIZONS]
    rows = []
    for label, g in [("all", f), ("buy", f[f["side"] == 1]), ("sell", f[f["side"] == -1])]:
        r = {"side": label, "n_fills": len(g), "qty": int(g["qty"].sum())}
        r.update(g[cols].mean().to_dict())
        r["pos_5m_pct"] = 100 * (g["mo_5m"] > 0).mean() if len(g) else np.nan
        r["mae_1m"] = g["mae_1m"].mean()
        r["mfe_1m"] = g["mfe_1m"].mean()
        rows.append(r)
    return pd.DataFrame(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--feed", required=True)
    ap.add_argument("--fills", required=True)
    ap.add_argument("--out", default="markouts.csv")
    a = ap.parse_args()
    f = markouts(pd.read_csv(a.feed), pd.read_csv(a.fills))
    print(f"markouts in ticks, {len(f)} fills")
    print(summary(f).round(3).to_string(index=False))
    f.to_csv(a.out, index=False)
    print(f"per-fill markouts written to {a.out}")


if __name__ == "__main__":
    main()
