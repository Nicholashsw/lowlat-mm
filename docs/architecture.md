# Architecture notes

This document captures the *why* behind the structure. The README covers
*what* and *how*; this is for when you're reading the code and wondering
"why like this".

## Event flow in detail

A backtest run is a single thread pumping a priority queue of events
ordered by `(timestamp, sequence)`:

1. Driver loads all market data events into the bus, each at its own
   timestamp.
2. Driver pops the next event. If it's a `TopOfBookEvent`:
   - update internal `OrderBook`,
   - call `ExchangeSimulator::on_market_data` (which may schedule fills
     for resting orders),
   - call `Strategy::on_market_data` (which may submit/cancel orders).
3. If it's a `NewOrderEvent` or `CancelOrderEvent`, dispatch to the
   simulator. The simulator schedules an ack at `ts + ack_latency_ns`
   and, for marketable orders, a fill at `ts + fill_latency_ns`.
4. If it's an `AckEvent` / `FillEvent` / `CancelAckEvent`, dispatch to
   the OMS and then notify the strategy.

The driver inserts latency at two points:
- **`accept_oms_event`**: when the OMS submits/cancels, the driver
  re-stamps the event with `ts + strategy_to_exchange_latency_ns` and
  pushes it onto the bus.
- **`accept_sim_event`**: when the simulator emits an ack/fill/cancel-ack,
  the driver re-stamps with `ts + exchange_to_strategy_latency_ns` and
  pushes it onto the bus.

This is what gives the engine its "the strategy and the exchange are at
opposite ends of a wire" feel without any threading.

## Why a `std::variant` for events

Three reasons:

1. **Closed set.** The event types are known at compile time. New event
   types are rare (we'd add `TradeEvent` if/when we model prints) and
   each addition deserves a compile-time review of every site that
   handles events. `std::visit` makes those sites visible — adding a new
   variant member without updating dispatch sites is a compile error.

2. **No heap allocations on the hot path.** A `unique_ptr<Event>`
   hierarchy would heap-allocate every event. With variant + small
   structs, every event lives on the stack of the bus.

3. **Type-driven dispatch reads better than virtual.** With
   `if constexpr` inside the visitor, the per-type handling code is
   right there, in one function, not scattered across overrides.

The cost is that every consumer must `std::visit`. That's a fair price
for a hot path; for cold paths (e.g., serialising events for replay) it
is negligible.

## Why integer-tick prices

Two related reasons:

1. **Equality.** Order book lookups by price are equality comparisons.
   Using `double` would either require fuzzy comparison or hidden
   rounding. Integers are exact.
2. **Cache.** Prices appear in `std::map<Price, Qty>` keys, in event
   structs, and in fill records. Keeping them as 8-byte ints rather
   than 8-byte doubles costs nothing and avoids any ambiguity at the
   API boundary.

Conversion to `double` happens only at the edges (logging, summary,
optional CSV output) where readability matters more than precision.

## Why the OMS uses a callback rather than a queue handle

The OMS doesn't know whether it's running in a backtest, a simulator-only
unit test, or against a real exchange connector. Its sole job is to
maintain order state. The mechanism for "send this event somewhere" is
a `std::function<void(const Event&)>` set at construction (or rebound
via `set_send`).

In the backtest the callback is `BacktestDriver::accept_oms_event`
which adds latency and pushes onto the bus. In a unit test the callback
is `events.push_back(e)`. In a hypothetical live mode it would be a
network connector. None of those changes touch the OMS itself.

## Why the simulator holds its own copy of the order book

Two distinct books exist:

- The driver's `OrderBook` is the strategy's view: "what does the
  market look like *to my strategy*?".
- The simulator's `books_[sym]` is the exchange's view: "what does
  the market look like for the purposes of deciding whether resting
  orders fill?".

In the current simulator they happen to receive identical input, but
keeping them separate is the right boundary. A future change — for
example, to model that the strategy sees data with extra wire latency
relative to the simulator's own state — slots in cleanly.

## Why no threading

For a single-machine backtest, threading buys nothing and costs
determinism. Every interesting concurrency phenomenon in real trading
(out-of-order acks, races between cancel and fill, message reordering)
can be modelled inside the latency parameters of the deterministic
single-threaded design.

A live mode would obviously need threads (a network thread feeding the
bus, a timer thread for housekeeping). The bus interface is thread-safe
in spirit — it's a queue with stable ordering — but the current
implementation is not lock-protected. Adding that is a one-day task if
ever needed.

## Inventory-aware market making — derivation

The strategy uses a simple inventory-skew quote:

```
reservation_px = mid - inv_skew * position
target_bid     = floor(reservation_px - half_spread)
target_ask     = ceil (reservation_px + half_spread)
```

This is the simplest cousin of the Avellaneda-Stoikov formulation. The
intuition: when long, lower both quotes so the ask gets hit more easily
(reducing inventory) and the bid is unlikely to fill (avoiding adding
to inventory). When short, raise both. With `inv_skew = 0` it's a pure
symmetric quoter around the mid.

`requote_thresh` is the tick distance the target must move before we
cancel-and-replace; it dampens churn from noise that doesn't matter.

`max_position` clamps both sides: if a fill would push us past the
limit on a side, that side is suppressed for this tick. The risk
manager enforces this independently as a defence in depth.

## Risk: belt and braces

The strategy does its own clamp on `max_position`. The risk manager
also enforces the same limit. Both checks should agree, but if they
ever disagree the risk manager wins — a bug in the strategy can't
breach the limit, only cause it to refuse to quote.

The kill switch is one-directional (trip → block) by design. Reset is
an explicit operator action, not an automatic recovery. The drawdown
trigger uses peak equity as the watermark, computed as
`realized_pnl + unrealized_pnl_at_last_mark`.

## What's deliberately not here

- **Order types beyond limit.** Stop, IOC, FOK, peg are not modelled.
- **Self-trade prevention.** Single strategy means we never quote
  against ourselves; with multiple strategies this would matter.
- **Position-level PnL across symbols.** Current PnL is per-symbol; a
  portfolio aggregator could be added in the driver.
- **Persistent state.** Every backtest starts flat. There's no
  serialization of state across runs.

These are all reasonable next steps; none of them require restructuring
what's already here.
