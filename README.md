# lowlat-mm

A modular C++20 prototype of an event-driven market-making and execution engine,
built for learning and as a portfolio piece. The goal is *not* a production
HFT system; it is a clean educational implementation of the patterns you would
find in one — order book, OMS, risk gate, exchange simulator, replay backtest
— with realistic boundaries between modules and reasonable defaults.

The codebase is small enough to read in a sitting and structured to be
extended (more strategies, deeper book, real feed plug-ins) without rewriting
the core.

## Architecture

```
                    ┌──────────────────┐
                    │  CSV feed reader │
                    └────────┬─────────┘
                             │ TopOfBookEvent
                             ▼
┌────────────┐   md   ┌────────────────┐   new/cancel    ┌──────────┐
│  Strategy  │◀──────│  BacktestDriver │────────────────▶│   OMS    │
│ (MarketMkr)│       │   (event loop)  │                 └────┬─────┘
└─────┬──────┘       │  + EventBus     │                      │
      │ submit/cancel│                 │◀─── ack/fill ────────┤
      ▼              │                 │                      ▼
   ┌──────┐          │                 │              ┌───────────────┐
   │ Risk │◀─────────┤                 │              │   Exchange    │
   └──────┘          └────────┬────────┘              │   Simulator   │
                              │ apply_top             └───────────────┘
                              ▼
                       ┌─────────────┐
                       │  OrderBook  │
                       └─────────────┘
```

Events flow on a single timestamp-ordered priority queue (`EventBus`). The
driver pumps the queue, dispatching each event to the right component. The
OMS emits `NewOrderEvent` / `CancelOrderEvent` which the driver re-schedules
with strategy→exchange latency; the simulator emits `AckEvent` / `FillEvent`
which the driver re-schedules with exchange→strategy latency. This single
queue plus pure dispatch keeps the backtest fully deterministic for a given
input.

Events are a `std::variant` rather than a polymorphic hierarchy: the set of
event types is closed and known at compile time, so `std::visit` gives both
better cache locality and explicit handling at every call site.

## Modules

| Folder        | Responsibility                                                   |
|---------------|------------------------------------------------------------------|
| `core/`       | Numeric types (tick-int prices), event variant, bus, logger      |
| `market_data/`| CSV feed reader and symbol interning                             |
| `orderbook/`  | Top-of-book + depth book (std::map for clarity)                  |
| `execution/`  | OMS: order state machine, fill handling, position tracking       |
| `risk/`       | Pre-trade checks (max position/order/open qty), kill switch, drawdown |
| `simulator/`  | Exchange simulator: ack latency, marketable fills, passive fills |
| `strategy/`   | Base interface + inventory-aware market maker                    |
| `backtest/`   | Driver, PnL/inventory time series, trade blotter                 |
| `apps/`       | `run_backtest` CLI                                               |
| `tests/`      | Catch2 unit tests across all modules                             |

## Why this exists

Because reading "how does an exchange work" articles is no substitute for
having a small, working, modifiable implementation. The interesting parts
of trading-systems engineering — event ordering, order state machines,
fill assumptions, latency modeling — only really click once you can change
something and see it break.

The same code structure also serves as a fair starting point for a real
research engine: a colocated feed handler can replace the CSV reader; a
real broker connector can replace the simulator; the strategy and risk
layers don't need to change.

## Build

Requirements:
- C++20 compiler (g++ 11+ or clang++ 13+)
- CMake 3.16+
- POSIX shell (macOS / Linux)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces:
- `build/run_backtest` — the CLI
- `build/unit_tests` — Catch2 test binary
- `build/liblowlat_mm_core.a` — the engine static library

## Run

The repo ships with three synthetic top-of-book feeds in `data/` and a
default config in `data/default.ini`.

```bash
# Random walk (5000 events, mean-reverting, std=3 noise)
./build/run_backtest data/default.ini data/sample_feed.csv

# Stable mid (1-tick jitter; smoke test, expect 0 fills)
./build/run_backtest data/default.ini data/stable_feed.csv

# Sine oscillation (friendliest for an MM)
./build/run_backtest data/default.ini data/oscillating_feed.csv
```

Optional outputs (configured in the `.ini`): a PnL/inventory time series
(`pnl.csv`) and a trade blotter (`trades.csv`).

## Sample output

Running `./build/run_backtest data/default.ini data/sample_feed.csv` on the
committed feed with the default config:

```
# loaded 5000 md events for symbol TEST

=== Backtest summary ===
symbol              : TEST
md events           : 5000
orders submitted    : 6157
fills               : 53
ending position     : -10
last mid (ticks)    : 9992
realized PnL        : -2.2500  (-225.0000 ticks)
unrealized PnL      : -0.3000  (-30.0000 ticks)
total PnL           : -2.5500  (-255.0000 ticks)
max drawdown        : 7.2500  (725.0000 ticks)
kill switch tripped : no
```

These numbers are exactly reproducible. `data/sample_feed.csv` is committed, and
`tools/generate_feed.py` regenerates it byte-for-byte from a fixed seed
(`random.Random(42)`, 5000 events); the engine itself is deterministic because
every event is ordered on a single timestamp-priority queue.

Negative PnL is expected here. See *Limitations* below — this is a real
adverse-selection effect, not a bug.

## Tests

```bash
./build/unit_tests           # 28 cases, ~109 assertions
ctest --test-dir build       # equivalent through CTest
```

Coverage includes order book updates, OMS state transitions and PnL
attribution, risk gate rejections (max position/order/open qty/drawdown,
kill switch), simulator fill rules under both passive-fill modes, and
strategy behaviour (no quotes on crossed/one-sided books, symmetric
quoting, inventory skew direction, max-position side suppression).

## Configuration

`data/default.ini` is a flat key=value file. Notable knobs:

```ini
# Latency model (nanoseconds)
strategy_to_exchange_latency_ns = 100000
exchange_to_strategy_latency_ns = 100000
ack_latency_ns                  = 50000
fill_latency_ns                 = 50000

# Passive fill rule (see Limitations)
fill_on_touch = true

# Risk
max_position       = 200
max_order_size     = 100
max_open_qty       = 500
max_drawdown_ticks = 1e12

# Strategy
half_spread_ticks = 2
min_spread_ticks  = 1
quote_size        = 10
inv_skew          = 0.05
requote_thresh    = 1
```

## Limitations and assumptions

This is an educational prototype. Honestly stated:

1. **Top-of-book only.** The feed format is `(bid_px, bid_sz, ask_px, ask_sz)`.
   Depth, queue position, and hidden liquidity are not modelled. The
   `OrderBook` class supports `apply_level()` for L2 updates, so plugging in
   a real L2 feed is mechanical, but the simulator currently uses only ToB.

2. **No trade prints.** A real exchange feed includes prints showing
   incoming aggressors. The simulator infers fills from quote movement,
   which understates fill rates for orders sitting at the touch on a real
   exchange.

3. **Passive fill model.** Two settings:
   - `fill_on_touch = true` (default): a resting buy at p fills when
     `best_ask ≤ p`. Realistic for an order improving the visible book
     (queue-priority winner). Optimistic for an order at the touch.
   - `fill_on_touch = false`: strict trade-through (`best_ask < p`).
     Conservative.
   On a TOB feed without prints, both rules under-fill an MM that quotes
   *inside* the visible market, because the strategy will typically cancel
   and requote as the mid moves before the market catches up to the old
   quote. This is the dominant reason the default backtest shows a loss:
   the only way the simulated MM gets filled is when the market trades
   *to or through* an old quote it hasn't yet cancelled, which is by
   definition adverse selection.

4. **Latency is fixed.** Real latency has jitter, queueing, and outliers.
   The model here adds a constant offset on each leg.

5. **Single-threaded, single-symbol.** The architecture is structured so
   multi-symbol is a small change (the OMS, risk, and book are already
   keyed by `SymbolId`); the strategy and runner currently assume one.

6. **No fees, no rebates, no borrow/funding.** PnL is purely
   price-difference on filled qty. Add a fee model in `OMS::on_fill`.

7. **No queue-position model.** A real MM's edge is heavily about how
   far back in the queue you are at each price level. We assume infinite
   liquidity at our limit price (full fill on touch, no partials from
   exhausted queue depth).

8. **The "low latency" in the project name is aspirational.** The hot
   path uses `std::map`, `std::unordered_map`, and `std::variant`, which
   is fine for a backtest but not appropriate for a real trading-loop
   binary. The patterns are right; the data structures would need
   replacing for a production system.

## Future work

Roughly in order of value-for-effort:

1. **Trade-print events.** Extend the feed format with optional trade
   prints; route them as a `TradeEvent` and let the simulator decide
   passive fills based on actual aggression rather than quote movement.
2. **Multiple strategies / multiple symbols.** Driver already keys
   everything by `SymbolId`; mostly a matter of registering N strategies.
3. **Latency jitter.** Replace constant latency with a `std::function<Timestamp()>`
   distribution.
4. **Queue-position model.** Track simulated queue ahead at each level,
   decay it on volume, deduce partial fills.
5. **Real feed plug-in.** Replace `CsvFeedReader` with a Databento or LOBSTER
   reader; the rest of the pipeline doesn't change.
6. **Better data structures on the hot path.** Replace `std::map` with
   tick-indexed arrays; replace `std::variant` dispatch with a function
   table if profiling shows it matters.
7. **More strategies.** Stoikov-style optimal market making, simple
   stat-arb, momentum execution.

## Repository layout

```
.
├── .gitignore
├── CMakeLists.txt
├── LICENSE
├── README.md
├── apps/
│   └── run_backtest.cpp            # CLI entry point
├── backtest/
│   ├── BacktestDriver.cpp          # event loop, PnL series, blotter
│   └── BacktestDriver.hpp
├── core/
│   ├── Config.hpp                  # flat key=value .ini reader
│   ├── EventBus.hpp                # timestamp-ordered priority queue
│   ├── Events.cpp
│   ├── Events.hpp                  # std::variant event set
│   ├── Logger.hpp
│   └── Types.hpp                   # tick-int prices, ids, timestamps
├── data/
│   ├── default.ini                 # baseline config
│   ├── aggressive.ini              # tighter quoting variants
│   ├── patient.ini
│   ├── short_patient.ini
│   ├── v_patient.ini
│   ├── tight.ini
│   ├── wide.ini
│   ├── debug.ini                   # trace-level logging
│   ├── sample_feed.csv             # noisy random walk
│   ├── stable_feed.csv             # 1-tick jitter (smoke test)
│   └── oscillating_feed.csv        # sine wave (MM-friendly)
├── docs/
│   └── architecture.md             # extended design notes
├── execution/
│   ├── OMS.cpp                     # order state machine, position, PnL
│   └── OMS.hpp
├── market_data/
│   ├── CsvFeedReader.cpp           # CSV reader, symbol interning
│   └── CsvFeedReader.hpp
├── orderbook/
│   ├── OrderBook.cpp               # top of book + depth
│   └── OrderBook.hpp
├── risk/
│   ├── RiskManager.cpp             # pre-trade gate, kill switch
│   └── RiskManager.hpp
├── simulator/
│   ├── ExchangeSimulator.cpp       # ack latency, fill rules
│   └── ExchangeSimulator.hpp
├── strategy/
│   ├── MarketMaker.cpp             # inventory-aware market maker
│   ├── MarketMaker.hpp
│   └── Strategy.hpp                # base interface
├── tests/
│   ├── test_main.cpp               # Catch2 main
│   ├── test_oms.cpp
│   ├── test_orderbook.cpp
│   ├── test_risk.cpp
│   ├── test_simulator.cpp
│   └── test_strategy.cpp
├── third_party/
│   └── catch.hpp                   # vendored single-header Catch2 v2.13.10
└── tools/
    ├── generate_feed.py            # synthetic feed generators (seeded)
    ├── generate_oscillating_feed.py
    ├── generate_stable_feed.py
    └── plot_pnl.py                 # pnl.csv -> chart
```

## Third-party

`third_party/catch.hpp` is the vendored single-header
[Catch2](https://github.com/catchorg/Catch2) v2.13.10 test framework, used
unmodified under the Boost Software License 1.0. Everything else in this
repository is covered by [`LICENSE`](LICENSE) (MIT).

---

Nicholas Hong | Built for educational and research purposes. Not financial advice.
