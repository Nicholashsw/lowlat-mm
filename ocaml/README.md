# lobsim: queue-aware replay in OCaml

The C++ engine's headline caveat is its fill rule: a resting order fills as soon as the market touches its price, as if it were first in the queue. On ESZ6 the touch held a median of about 25 contracts on each side, all ahead of a new order, so most of those fills would not have happened. `lobsim` is an OCaml replay of the same strategy that can say how many would.

It has three parts.

- **A price-time matching engine** (`lib/matching_engine.ml`): limit, market, cancel and size-reduce on a single book, FIFO within a level, O(1) cancels by lazy deletion. QCheck runs it against a deliberately naive reference book on random order flow and requires identical fills and an identical book after every step, plus three invariants: the book is never crossed, fills respect the limit and walk outwards, quantity is conserved.
- **A replay of the C++ backtest** (`lib/backtest.ml` and friends): same event ordering, latencies, OMS state machine, risk gate and market maker, streamed from the feed file instead of loaded into memory.
- **Two queue-position fill rules** (`lib/exchange.ml`) next to the C++ engine's two, driven by the exchange's trade prints.

## The replay reproduces the C++ engine

With the touch or through rule, `lobsim-replay` produces a byte-identical fill blotter and an identical summary to `build/run_backtest`. `crosscheck.sh` checks this for every config in `data/` on three synthetic feeds each (24 runs, up to 2,369 fills per run). It is about a hundred times faster on the same feed (0.2 s against 19.4 s for 200,000 events), mainly because the C++ OMS scans every order it has ever seen on each risk check, which makes a full session quadratic, while the OCaml OMS keeps live orders in their own table.

Writing the port turned up a bug in the C++ position accounting, now fixed in `execution/OMS.cpp` with two regression tests: partly covering a short reset the entry price to the cover price, and flipping from short to long kept the old short entry price. Both distort realised and unrealised PnL whenever the book is short, so the published ESZ6 figures need a re-run.

## The fill rules

| Rule | A resting buy fills when | Source |
|---|---|---|
| `touch` | the best offer is at or below its price, in full | C++ `fill_on_touch = true` |
| `through` | the best offer is below its price, in full | C++ `fill_on_touch = false` |
| `queue-behind` | prints at its price have consumed the queue that was ahead of it, partially if a print only reaches part of it; outright on a print or offer through its price | this module |
| `queue-proportional` | as `queue-behind`, but cancels at its price are spread over the whole queue | this module |

How the queue position is kept: an order joining the best price queues behind all the size displayed there; an order inside the spread is alone; an order behind the best has no known position until its price is the best again, when it goes behind everything shown. A print at the order's price is taken from the queue ahead first. A drop in displayed size larger than the prints at that price is cancellation: `queue-behind` assumes all of it came from behind the order, which is the pessimistic bound, and `queue-proportional` takes the share ahead / queue of it from in front. If the order's price disappears from its side of the book, everything ahead has gone and the order is first. `test/test_replay.ml` walks through each case.

What it cannot see: with top-of-book data the queue is only observable while the order's price is the best; orders the strategy itself would have added to the book are not in the data, so its own effect on others' queues is ignored; and the trade-before-quote rule at equal timestamps assumes CME's publication order.

## Build and run

Needs OCaml 4.14 or 5.x, dune and qcheck-core (`opam install dune qcheck-core`).

```bash
cd ocaml
dune build
dune test                                    # QCheck properties and scenario tests
./crosscheck.sh                              # after cmake --build ../build
dune exec lobsim-bench                       # matching engine throughput
```

On real data, pull the prints with the feed and run each rule on the same day:

```bash
python3 tools/databento_feed.py --date 2026-10-16 --symbol ESZ6 \
    --out data/es_feed.csv --trades-out data/es_trades.csv
for rule in touch through queue-proportional queue-behind; do
  ocaml/_build/default/bin/replay.exe data/es.ini data/es_feed.csv --trades data/es_trades.csv \
      --fill $rule --trades-out fills_$rule.csv
done
```

The blotters have the C++ `trades.csv` layout, so `analytics/markouts.q` measures each rule's adverse selection the same way.

`lobsim-bench` on random flow (60% limits near the touch, 30% cancels, 10% market orders, two million operations, about 200,000 orders left resting) runs at about 1.1 million operations a second, 0.9 µs each, on this machine. The book is a balanced map of levels, chosen for clarity; a production book would index levels by tick in an array.
