#pragma once

// Exchange simulator.
//
// Consumes NewOrderEvent / CancelOrderEvent from the strategy side and emits
// AckEvent / FillEvent / CancelAckEvent back. Fill logic is deliberately
// simple and clearly documented so assumptions are visible:
//
//   1. Orders are acked after a fixed ack_latency_ns (symmetric for new /
//      cancel).
//   2. A resting limit order fills when the opposite side of the top of
//      book trades through its price. Concretely, for each TopOfBookEvent:
//        - every resting BUY at px >= best_ask fills at its own limit px,
//        - every resting SELL at px <= best_bid fills at its own limit px.
//      That is: we assume that if the market *could* cross our resting
//      order, a counterparty did fill us at our limit. This is the
//      "optimistic passive fill" assumption.
//   3. Queue priority is not modelled: a resting order that sits at the
//      best price gets filled only if the market trades *through* it,
//      never at the touch. This is conservative for a market maker.
//   4. Fills are for the full remaining qty unless a per-order partial_size
//      override is configured (not used by default).
//   5. Marketable orders (crossing the book at submit time) are filled
//      immediately at the resting price of the opposite side.
//
// These are simplifications. A serious simulator would model queue
// position, hidden liquidity, exchange fees, latency jitter, cancel-reject
// races, and self-trade prevention.

#include "../core/Types.hpp"
#include "../core/Events.hpp"
#include "../core/EventBus.hpp"
#include "../orderbook/OrderBook.hpp"
#include <unordered_map>
#include <vector>
#include <functional>

namespace lmm {

struct SimulatorConfig {
    Timestamp ack_latency_ns   = 50'000;   // 50 microseconds
    Timestamp fill_latency_ns  = 50'000;
    bool      allow_marketable = true;
    // Passive-fill rule for resting orders.
    //  - true  (default): "fill on touch" — a resting BUY at p fills when
    //                     best_ask <= p; a resting SELL at p fills when
    //                     best_bid >= p. Realistic for an order that
    //                     improves the visible book (queue-priority winner).
    //                     Optimistic for an order sitting at the touch.
    //  - false: "trade through only" — strict inequality. Conservative;
    //                     resting orders only fill when the market clearly
    //                     prints through them.
    bool      fill_on_touch    = true;
};

class ExchangeSimulator {
public:
    using EmitFn = std::function<void(Timestamp, Event)>;

    ExchangeSimulator(SimulatorConfig cfg, EmitFn emit)
        : cfg_(cfg), emit_(std::move(emit)) {}

    // Called when the strategy / OMS has submitted a new order. Schedules
    // an ack, and if the order is marketable, also schedules an immediate
    // fill.
    void on_new_order(const NewOrderEvent& e);
    void on_cancel_order(const CancelOrderEvent& e);

    // Called on every market data update. Advances the reference book and
    // fills any resting orders that are now through the market.
    void on_market_data(const TopOfBookEvent& md);

    std::size_t live_orders() const { return resting_.size(); }

private:
    struct Resting {
        NewOrderEvent ev;
        Qty leaves;
    };

    void maybe_fill_against_book(Resting& r, Timestamp ts);
    void emit_fill(const Resting& r, Price px, Qty qty, Timestamp ts, bool is_final);

    SimulatorConfig cfg_;
    EmitFn emit_;
    std::unordered_map<SymbolId, OrderBook> books_;
    std::unordered_map<OrderId, Resting>    resting_;
};

} // namespace lmm
