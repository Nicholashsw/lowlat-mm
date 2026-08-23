#include "RiskManager.hpp"
#include "../core/Logger.hpp"
#include <cstdlib>

namespace lmm {

bool RiskManager::check_new_order(SymbolId sym, Side side, Qty qty,
                                  std::string& reason) const {
    if (killed_) { reason = "kill_switch"; return false; }
    if (qty <= 0) { reason = "non_positive_qty"; return false; }
    if (qty > lim_.max_order_size) { reason = "order_size"; return false; }

    const auto& pos = oms_.position(sym);
    // Worst-case projection: assume the whole new order fills AND all open
    // orders on the same side also fill. We deliberately over-estimate to be
    // conservative.
    Qty signed_qty = (side == Side::Buy ? qty : -qty);

    // Sum leaves on the same side.
    Qty same_side_leaves = 0;
    for (auto* o : oms_.open_orders(sym)) {
        if (o->side == side) same_side_leaves += o->leaves();
    }
    Qty signed_same_side = (side == Side::Buy ? same_side_leaves : -same_side_leaves);

    Qty projected = pos.net + signed_qty + signed_same_side;
    if (std::abs(projected) > lim_.max_position) {
        reason = "max_position";
        return false;
    }

    if (oms_.gross_open_qty(sym) + qty > lim_.max_open_qty) {
        reason = "max_open_qty";
        return false;
    }

    return true;
}

void RiskManager::mark_equity(double equity_ticks) {
    if (!has_peak_) { peak_equity_ = equity_ticks; has_peak_ = true; }
    if (equity_ticks > peak_equity_) peak_equity_ = equity_ticks;
    double drawdown = peak_equity_ - equity_ticks;
    if (drawdown > lim_.max_drawdown_ticks && !killed_) {
        kill("max_drawdown");
    }
}

void RiskManager::kill(const std::string& reason) {
    if (!killed_) {
        killed_ = true;
        LMM_LOG(Warn, "risk") << "KILL SWITCH tripped: " << reason;
    }
}

} // namespace lmm
