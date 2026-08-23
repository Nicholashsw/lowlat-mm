#pragma once

// Pre-trade risk manager.
//
// Sits between the strategy and the OMS: the strategy asks the risk layer
// "can I submit this order?" and the risk layer answers yes or no based on:
//   - max absolute position per symbol,
//   - max single-order size,
//   - max gross open notional per symbol,
//   - max drawdown (realised + unrealised PnL at last mark),
//   - global kill switch.
//
// Once the kill switch is tripped, the risk layer rejects all new orders
// until explicitly reset. Existing live orders are not auto-cancelled here;
// the backtest driver will call a helper for that.

#include "../core/Types.hpp"
#include "../execution/OMS.hpp"
#include <string>

namespace lmm {

struct RiskLimits {
    Qty    max_position      = 1000;   // per symbol, absolute
    Qty    max_order_size    = 100;    // per single order
    Qty    max_open_qty      = 500;    // gross leaves per symbol
    double max_drawdown_ticks = 1e12;  // stop trading below this PnL
};

class RiskManager {
public:
    RiskManager(const RiskLimits& lim, const OMS& oms) : lim_(lim), oms_(oms) {}

    // Returns true if ok. If false, reason is populated with a short code.
    bool check_new_order(SymbolId sym, Side side, Qty qty,
                         std::string& reason) const;

    // Runtime check: called periodically with current equity (realised +
    // unrealised PnL in ticks). Trips the kill switch on breach.
    void mark_equity(double equity_ticks);

    bool killed() const { return killed_; }
    void kill(const std::string& reason);
    void reset() { killed_ = false; }

    const RiskLimits& limits() const { return lim_; }
    double peak_equity_ticks() const { return peak_equity_; }

private:
    RiskLimits lim_;
    const OMS& oms_;
    bool killed_ = false;
    double peak_equity_ = 0.0;
    bool has_peak_ = false;
};

} // namespace lmm
