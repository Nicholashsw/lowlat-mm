#!/usr/bin/env python3
"""
Pull one day of CME ES top of book from Databento and write it in the
engine's CSV format.

Usage:
    export DATABENTO_API_KEY=...
    python3 tools/databento_feed.py [--date YYYY-MM-DD] [--symbol ESZ6] [--out data/es_feed.csv]
                                    [--trades-out data/es_trades.csv]

Dataset GLBX.MDP3, schema mbp-1, regular hours only (13:30 to 20:00 UTC).
The date defaults to the last weekday before today. Prints the Databento
cost first and stops if it is over US$10.

Output columns: ts_ns, symbol, bid_px, bid_sz, ask_px, ask_sz, with prices
in ES ticks (0.25 index points). A row is written only when the top of book
changes; rows with a missing or crossed side are skipped.

--trades-out also writes the trade prints from the same records (mbp-1 carries
them as action T): ts_ns, symbol, side, px, sz, with side the aggressor (B buy,
S sell) and px in ticks. The OCaml replay's queue fill rules read this file; the
C++ engine does not need it.

Requires: pip install databento
"""
import argparse
import csv
import os
import sys
from datetime import date, timedelta

import databento as db

DATASET = "GLBX.MDP3"
SCHEMA = "mbp-1"
MAX_COST_USD = 10.0
TICK_FIXED = 250_000_000  # 0.25 in Databento fixed-point prices (1e-9 units)


def last_weekday_before(d: date) -> date:
    d -= timedelta(days=1)
    while d.weekday() >= 5:
        d -= timedelta(days=1)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", default=last_weekday_before(date.today()).isoformat())
    ap.add_argument("--symbol", default="ESZ6")
    ap.add_argument("--out", default="data/es_feed.csv")
    ap.add_argument("--trades-out", help="also write trade prints here")
    args = ap.parse_args()

    key = os.environ.get("DATABENTO_API_KEY")
    if not key:
        sys.exit("DATABENTO_API_KEY is not set")

    start = f"{args.date}T13:30:00Z"
    end = f"{args.date}T20:00:00Z"
    query = dict(dataset=DATASET, symbols=[args.symbol], schema=SCHEMA,
                 stype_in="raw_symbol", start=start, end=end)

    client = db.Historical(key)
    cost = client.metadata.get_cost(**query)
    print(f"Databento cost for {args.symbol} {DATASET} {SCHEMA} {start} to {end}: US${cost:.4f}")
    if cost > MAX_COST_USD:
        sys.exit(f"cost is over US${MAX_COST_USD:.2f}, stopping")

    store = client.timeseries.get_range(**query)

    cols = ["bid_px_00", "bid_sz_00", "ask_px_00", "ask_sz_00"]
    last = None
    n_in = n_out = n_bad = n_trades = 0
    tf = open(args.trades_out, "w", newline="") if args.trades_out else None
    tw = csv.writer(tf) if tf else None
    if tw:
        tw.writerow(["ts_ns", "symbol", "side", "px", "sz"])
    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ts_ns", "symbol", "bid_px", "bid_sz", "ask_px", "ask_sz"])
        # Fixed-point prices and integer timestamps, read in chunks to keep memory flat.
        for df in store.to_df(price_type="fixed", pretty_ts=False, map_symbols=False, count=1_000_000):
            df = df.reset_index()
            trade_cols = (df["action"], df["side"], df["price"], df["size"]) if tw else ()
            for ts, bp, bs, ap_, as_, *trade in zip(df["ts_recv"], *(df[c] for c in cols), *trade_cols):
                if trade and trade[0] == "T" and trade[1] in ("A", "B") and trade[2] != db.UNDEF_PRICE:
                    # Databento marks the aggressor: B buy, A (ask side) sell.
                    tw.writerow([int(ts), args.symbol, "B" if trade[1] == "B" else "S",
                                 trade[2] // TICK_FIXED, int(trade[3])])
                    n_trades += 1
                n_in += 1
                # Skip a missing side (undefined price or zero size) or a crossed or locked book.
                if bp == db.UNDEF_PRICE or ap_ == db.UNDEF_PRICE or bs <= 0 or as_ <= 0 or bp >= ap_:
                    n_bad += 1
                    continue
                row = (bp // TICK_FIXED, int(bs), ap_ // TICK_FIXED, int(as_))
                if row == last:
                    continue
                last = row
                w.writerow([int(ts), args.symbol, *row])
                n_out += 1

    if tf:
        tf.close()
        print(f"trade prints written: {n_trades} to {args.trades_out}")
    print(f"records read: {n_in}, skipped (missing or crossed): {n_bad}, top-of-book changes written: {n_out}")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
