#!/usr/bin/env python3
"""
The quoting-rule test pre-registered in results/quoting/PREREG.md.

    python3 tools/quoting_study.py --data-dir DIR

DIR holds the cached sessions written by tools/multiday.py (es_feed_DAY.csv[.gz] and
es_trades_DAY.csv[.gz]); nothing is pulled from Databento. Steps:

  1. grid: 27 combinations of k, L and theta on the 10 training sessions, queue-behind
  2. pick the highest mean daily PnL (ties: smaller k, smaller L, larger theta)
  3. test: baseline and the chosen rule, once, on the 11 test sessions, all four fill
     rules, with markouts and the regression of daily PnL on ES's open-to-close move

Writes aggregates only to results/quoting/: grid.csv, test_days.csv, test_summary.csv.
Blotters and decompressed feeds stay in DIR.
"""
import argparse
import gzip
import itertools
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "analytics"))
sys.path.insert(0, str(ROOT / "tools"))
from markouts import markouts  # noqa: E402
from multiday import TICK_PTS, TICK_USD, ols, ticks  # noqa: E402

REPLAY = ROOT / "ocaml" / "_build" / "default" / "bin" / "replay.exe"
INI = ROOT / "data" / "es.ini"
OUT = ROOT / "results" / "quoting"
RULES = ["touch", "through", "queue-proportional", "queue-behind"]
TRAIN = [d.strftime("%Y-%m-%d") for d in pd.bdate_range("2026-09-18", "2026-10-01")]
TEST = [d.strftime("%Y-%m-%d") for d in pd.bdate_range("2026-10-02", "2026-10-16")]
GRID = list(itertools.product([0, 1, 2], [10, 20, 40], [0.3, 0.5, 0.7]))   # k, L, theta
HORIZONS = ["0ms", "1s", "1m", "5m"]


def session(data, day):
    """Plain CSV paths for a day's quotes and prints, decompressing a cached .gz once."""
    paths = []
    for stem in ("es_feed", "es_trades"):
        p = data / f"{stem}_{day}.csv"
        if not p.exists():
            gz = p.with_suffix(".csv.gz")
            if not gz.exists():
                sys.exit(f"{day}: {p.name} is not cached in {data}; no new pulls")
            with gzip.open(gz, "rb") as src, open(p, "wb") as dst:
                shutil.copyfileobj(src, dst)
        paths.append(p)
    return paths


def replay(data, day, rule, micro=None, blotter=None):
    feed, prints = session(data, day)
    cmd = [str(REPLAY), str(INI), str(feed), "--trades", str(prints), "--fill", rule]
    if micro:
        cmd += ["--micro", ",".join(map(str, micro))]
    if blotter:
        cmd += ["--trades-out", str(blotter)]
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    return ticks(out, "total PnL") * TICK_USD, ticks(out, "max drawdown") * TICK_USD


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--jobs", type=int, default=6)
    data = Path(ap.parse_args().data_dir).resolve()
    jobs = ap.parse_args().jobs
    if not REPLAY.exists():
        sys.exit(f"{REPLAY} not built: cd ocaml && dune build")
    OUT.mkdir(parents=True, exist_ok=True)
    for d in TRAIN + TEST:
        session(data, d)

    # 1-2. grid on the training sessions, queue-behind
    tasks = [(g, d) for g in GRID for d in TRAIN]
    with ThreadPoolExecutor(jobs) as ex:
        pnl = list(ex.map(lambda gd: replay(data, gd[1], "queue-behind", micro=gd[0])[0], tasks))
    grid = (pd.DataFrame([{"k": g[0], "L": g[1], "theta": g[2], "day": d, "pnl_usd": p}
                          for (g, d), p in zip(tasks, pnl)])
              .groupby(["k", "L", "theta"])["pnl_usd"].agg(mean_daily_pnl_usd="mean", days="size").reset_index())
    grid = grid.sort_values(["mean_daily_pnl_usd", "k", "L", "theta"], ascending=[False, True, True, False])
    grid.round(2).to_csv(OUT / "grid.csv", index=False)
    best = grid.iloc[0]
    chosen = (int(best.k), int(best.L), float(best.theta))
    print(f"chosen on training sessions: k={chosen[0]} L={chosen[1]} theta={chosen[2]} "
          f"(mean daily PnL US${best.mean_daily_pnl_usd:,.0f} under queue-behind)")

    # 3. test sessions, once: both rules, all four fill rules
    runs = [(q, r, d) for q in ("baseline", "micro") for r in RULES for d in TEST]

    def one(qrd):
        q, r, d = qrd
        blotter = data / f"fills_{q}_{r}_{d}.csv"
        p, dd = replay(data, d, r, micro=chosen if q == "micro" else None, blotter=blotter)
        return {"quoting": q, "rule": r, "day": d, "pnl_usd": p, "max_dd_usd": dd}

    with ThreadPoolExecutor(jobs) as ex:
        days = pd.DataFrame(list(ex.map(one, runs)))

    oc, mo = {}, []
    for d in TEST:
        quotes = pd.read_csv(data / f"es_feed_{d}.csv")
        mid = (quotes["bid_px"] + quotes["ask_px"]) / 2
        oc[d] = (mid.iloc[-1] - mid.iloc[0]) * TICK_PTS
        for q, r in itertools.product(("baseline", "micro"), RULES):
            f = pd.read_csv(data / f"fills_{q}_{r}_{d}.csv")
            m = markouts(quotes, f) if len(f) else f
            marks = {f"mo_{h}": m[f"mo_{h}"].to_numpy() for h in HORIZONS} if len(f) else {}
            mo.append({"quoting": q, "rule": r, "day": d, "fills": len(f),
                       "contracts": int(f["qty"].sum()) if len(f) else 0, **marks})
    days["es_oc_pts"] = days["day"].map(oc)
    mo = pd.DataFrame(mo)
    days = days.merge(mo[["quoting", "rule", "day", "fills", "contracts"]], on=["quoting", "rule", "day"])
    days.round(4).to_csv(OUT / "test_days.csv", index=False)

    rows = []
    for (q, r), g in days.groupby(["quoting", "rule"], sort=False):
        y, x = g["pnl_usd"].to_numpy(), g["es_oc_pts"].to_numpy()
        (a, b), (ta, tb) = ols(y, x)
        allm = mo[(mo.quoting == q) & (mo.rule == r)]
        row = {"quoting": q, "rule": r, "k": chosen[0] if q == "micro" else None,
               "L": chosen[1] if q == "micro" else None, "theta": chosen[2] if q == "micro" else None,
               "days": len(g), "fills": int(g["fills"].sum()), "contracts": int(g["contracts"].sum()),
               "mean_pnl_usd": y.mean(), "median_pnl_usd": np.median(y),
               "t_mean": y.mean() / (y.std(ddof=1) / np.sqrt(len(y))) if y.std(ddof=1) > 0 else np.nan,
               "days_positive": int((y > 0).sum()),
               "slope_usd_per_pt": b, "t_slope": tb, "intercept_usd": a, "t_intercept": ta,
               "r2": np.corrcoef(y, x)[0, 1] ** 2 if y.std() > 0 else np.nan}
        for h in HORIZONS:
            vals = np.concatenate([v for v in allm.get(f"mo_{h}", []) if isinstance(v, np.ndarray)] or [np.array([])])
            row[f"mo_{h}"] = vals.mean() if len(vals) else np.nan     # mean ticks per fill, all test days
        if q == "micro":
            base = days[(days.quoting == "baseline") & (days.rule == r)].set_index("day")["pnl_usd"]
            dif = g.set_index("day")["pnl_usd"] - base
            row["diff_vs_baseline_mean_usd"] = dif.mean()
            row["diff_vs_baseline_t"] = dif.mean() / (dif.std(ddof=1) / np.sqrt(len(dif)))
        rows.append(row)
    summ = pd.DataFrame(rows)
    summ.round(4).to_csv(OUT / "test_summary.csv", index=False)
    print(summ.round(2).to_string(index=False))

    qb = summ.set_index(["quoting", "rule"])
    beats = all(qb.loc[("micro", "queue-behind"), f"mo_{h}"] > qb.loc[("baseline", "queue-behind"), f"mo_{h}"]
                for h in ("1s", "1m", "5m"))
    print(f"port to C++ (micro beats baseline markouts at 1s, 1m and 5m under queue-behind): {'yes' if beats else 'no'}")


if __name__ == "__main__":
    main()
