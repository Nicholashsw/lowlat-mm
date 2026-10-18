#!/usr/bin/env python3
"""
Run the OCaml replay under all four fill rules on a range of ES sessions.

    export DATABENTO_API_KEY=...
    python3 tools/multiday.py --start 2026-09-18 --end 2026-10-16 --data-dir DIR

For each weekday in the range it pulls the session with tools/databento_feed.py
(quotes and trade prints, regular hours), unless DIR already holds that day's
files, then replays it with ocaml/_build/default/bin/replay.exe for touch,
through, queue-proportional and queue-behind, and measures each blotter's
markouts with analytics/markouts.py. Each day starts flat.

Market data, blotters and per-fill markouts stay in DIR, which must be outside
the repository. The repo gets two aggregate files:
  results/multiday_summary.csv   day, rule, fills, contracts, PnL USD, max DD USD, and ES's
                                 open-to-close change in points (first to last mid of the session)
  results/multiday_markouts.csv  day, rule, mean markout per fill at 0 ms, 1 s, 1 m, 5 m
  results/multiday_by_rule.csv   per rule: mean, median and t-stat of daily PnL, days positive,
                                 and the OLS of daily PnL on the open-to-close change

Stops before pulling anything if Databento's cost estimate for the days still
to pull is over --max-cost (US$30 by default).
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "analytics"))
from markouts import markouts  # noqa: E402

RULES = ["touch", "through", "queue-proportional", "queue-behind"]
TICK_USD = 12.50          # one ES tick, per contract
TICK_PTS = 0.25           # one ES tick in index points
REPLAY = ROOT / "ocaml" / "_build" / "default" / "bin" / "replay.exe"


def session_cost(days, symbol):
    import databento as db
    c = db.Historical(os.environ["DATABENTO_API_KEY"])
    return sum(c.metadata.get_cost(dataset="GLBX.MDP3", symbols=[symbol], schema="mbp-1",
                                   stype_in="raw_symbol", start=f"{d}T13:30:00Z", end=f"{d}T20:00:00Z")
               for d in days)


def ticks(text, field):
    """The tick figure in brackets on a replay summary line, e.g. 'total PnL : 602.5 (2410.0 ticks)'."""
    m = re.search(rf"^{field}\s*:.*\(\s*(-?[\d.]+) ticks\)", text, re.M)
    if not m:
        raise RuntimeError(f"no '{field}' line in the replay output")
    return float(m.group(1))


def ols(y, x):
    """Intercept and slope of y on x with plain OLS t-stats."""
    X = np.column_stack([np.ones(len(x)), x])
    b, *_ = np.linalg.lstsq(X, y, rcond=None)
    e = y - X @ b
    se = np.sqrt(np.diag(e @ e / (len(y) - 2) * np.linalg.inv(X.T @ X)))
    return b, b / se


def by_rule(summary):
    rows = []
    for rule, g in summary.groupby("rule", sort=False):
        y, x = g["pnl_usd"].to_numpy(), g["es_oc_pts"].to_numpy()
        (a, b), (ta, tb) = ols(y, x)
        rows.append({"rule": rule, "days": len(g), "mean_pnl_usd": y.mean(), "median_pnl_usd": np.median(y),
                     "days_positive": int((y > 0).sum()), "t_mean": y.mean() / (y.std(ddof=1) / np.sqrt(len(y))),
                     "slope_usd_per_pt": b, "t_slope": tb, "intercept_usd": a, "t_intercept": ta})
    return pd.DataFrame(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", required=True)
    ap.add_argument("--end", required=True)
    ap.add_argument("--symbol", default="ESZ6")
    ap.add_argument("--data-dir", required=True)
    ap.add_argument("--max-cost", type=float, default=30.0)
    a = ap.parse_args()

    data = Path(a.data_dir).resolve()
    if ROOT in data.parents or data == ROOT:
        sys.exit("--data-dir must be outside the repository")
    if not REPLAY.exists():
        sys.exit(f"{REPLAY} not built: cd ocaml && dune build")
    data.mkdir(parents=True, exist_ok=True)

    days = [d.strftime("%Y-%m-%d") for d in pd.bdate_range(a.start, a.end)]
    feed = lambda d: data / f"es_feed_{d}.csv"
    prints = lambda d: data / f"es_trades_{d}.csv"
    to_pull = [d for d in days if not (feed(d).exists() and prints(d).exists())]
    if to_pull:
        cost = session_cost(to_pull, a.symbol)
        print(f"Databento estimate for {len(to_pull)} days to pull: US${cost:.2f}")
        if cost > a.max_cost:
            sys.exit(f"over US${a.max_cost:.2f}, stopping")

    summary, marks = [], []
    for d in days:
        if d in to_pull:
            subprocess.run([sys.executable, str(ROOT / "tools" / "databento_feed.py"), "--date", d,
                            "--symbol", a.symbol, "--out", str(feed(d)), "--trades-out", str(prints(d))],
                           check=True)
        quotes = pd.read_csv(feed(d))
        if quotes.empty:
            sys.exit(f"{d}: empty feed")
        mid = (quotes["bid_px"] + quotes["ask_px"]) / 2
        oc_pts = (mid.iloc[-1] - mid.iloc[0]) * TICK_PTS
        for rule in RULES:
            blotter = data / f"fills_{rule}_{d}.csv"
            out = subprocess.run([str(REPLAY), str(ROOT / "data" / "es.ini"), str(feed(d)),
                                  "--trades", str(prints(d)), "--fill", rule, "--trades-out", str(blotter)],
                                 check=True, capture_output=True, text=True).stdout
            fills = pd.read_csv(blotter)
            summary.append({"day": d, "rule": rule, "fills": len(fills), "contracts": int(fills["qty"].sum()),
                            "pnl_usd": ticks(out, "total PnL") * TICK_USD,
                            "max_dd_usd": ticks(out, "max drawdown") * TICK_USD, "es_oc_pts": oc_pts})
            m = markouts(quotes, fills)
            m.to_csv(data / f"markouts_{rule}_{d}.csv", index=False)
            marks.append({"day": d, "rule": rule, "fills": len(m),
                          **{f"mo_{h}": m[f"mo_{h}"].mean() for h in ("0ms", "1s", "1m", "5m")}})
            print(f"{d} {rule:19s} fills {len(fills):4d}  PnL US${summary[-1]['pnl_usd']:>11,.2f}")

    res = ROOT / "results"
    res.mkdir(exist_ok=True)
    summary = pd.DataFrame(summary)
    summary.to_csv(res / "multiday_summary.csv", index=False)
    pd.DataFrame(marks).round(4).to_csv(res / "multiday_markouts.csv", index=False)
    rules = by_rule(summary)
    rules.round(4).to_csv(res / "multiday_by_rule.csv", index=False)
    print(rules.round(2).to_string(index=False))
    print(f"wrote multiday_summary.csv, multiday_markouts.csv and multiday_by_rule.csv in {res}")


if __name__ == "__main__":
    main()
