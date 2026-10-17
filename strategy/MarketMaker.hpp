#pragma once

// Simple inventory-aware market making strategy.
//
// Behaviour per tick:
//   1. If the book is one-sided, crossed, or the spread is below a floor,
//      cancel everything and do nothing.
//   2. Compute a reservation price by skewing the mid against inventory:
//        r = mid - skew_ticks_per_unit * position
//      Being long biases us to quote lower (ask gets more aggressive);
//      being short biases us higher.
//   3. Compute quote prices:
//        bid = round_down(r - half_spread)
//        ask = round_up  (r + half_spread)
//      ensuring bid < ask and both inside the visible book.
//   4. If an existing working order is close enough to the target, leave
//      it. Otherwise cancel and re-quote. This avoids churn from tiny
//      mid moves.
//   5. Refuse to quote past max_position.
//
// Notation is in tick units throughout.

#include "Strategy.hpp"

namespace lmm {

struct MMConfig {
    SymbolId sym             = 0;
    Price    half_spread     = 2;    // ticks
    Price    min_spread      = 1;    // refuse to quote if market spread <= this
    Qty      quote_size      = 10;
    Qty      max_position    = 200;
    double   inv_skew        = 0.01; // tick skew per unit of inventory
    Price    requote_thresh  = 1;    // re-quote only if target moves >= this
};

class MarketMaker : public Strategy {
public:
    MarketMaker(MMConfig cfg, OMS& oms, RiskManager& risk)
        : cfg_(cfg), oms_(oms), risk_(risk) {}

    void on_market_data(const TopOfBookEvent& md,
                        const OrderBook& book) override;

    const char* name() const override { return "MarketMaker"; }

private:
    // ts is the simulated time of the event that triggered the cancel.
    // It must be a real event timestamp: the driver schedules the cancel at
    // ts + strategy_to_exchange_latency, so passing 0 here would place the
    // arrival in the past and pop it out of chronological order.
    void cancel_all(Timestamp ts);
    void try_submit(Timestamp ts, Side side, Price px);

    MMConfig cfg_;
    OMS& oms_;
    RiskManager& risk_;

    // Track our currently-live quote prices per side (0 = none).
    OrderId live_bid_id_ = 0;
    OrderId live_ask_id_ = 0;
    Price   live_bid_px_ = kInvalidPrice;
    Price   live_ask_px_ = kInvalidPrice;
};

} // namespace lmm
