#include "OMS.hpp"
#include "../core/Logger.hpp"

namespace lmm {

OrderId OMS::submit(Timestamp ts, SymbolId sym, Side side, Price px, Qty qty) {
    OrderRecord r{};
    r.id = next_id_++;
    r.sym = sym;
    r.side = side;
    r.px = px;
    r.qty = qty;
    r.filled_qty = 0;
    r.status = OrderStatus::PendingNew;
    r.created_ts = ts;
    r.last_update_ts = ts;
    orders_[r.id] = r;

    NewOrderEvent e{r.id, ts, sym, side, px, qty};
    send_(Event{e});
    LMM_LOG(Debug, "oms") << "submit id=" << r.id << " " << side_str(side)
                          << " px=" << px << " qty=" << qty;
    return r.id;
}

bool OMS::cancel(Timestamp ts, OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    auto& o = it->second;
    if (o.is_terminal()) return false;
    if (o.status == OrderStatus::PendingCancel) return false;

    o.status = OrderStatus::PendingCancel;
    o.last_update_ts = ts;
    CancelOrderEvent e{id, ts};
    send_(Event{e});
    LMM_LOG(Debug, "oms") << "cancel id=" << id;
    return true;
}

void OMS::on_ack(const AckEvent& e) {
    auto it = orders_.find(e.id);
    if (it == orders_.end()) {
        LMM_LOG(Warn, "oms") << "ack for unknown id=" << e.id;
        return;
    }
    auto& o = it->second;
    o.last_update_ts = e.ts;
    if (e.accepted) {
        // Only transition to Working if we weren't already cancelling or
        // partially filled (an ack can race with a fill).
        if (o.status == OrderStatus::PendingNew) {
            o.status = OrderStatus::Working;
        }
    } else {
        o.status = OrderStatus::Rejected;
        LMM_LOG(Info, "oms") << "reject id=" << e.id << " reason=" << e.reason;
    }
}

void OMS::on_fill(const FillEvent& e) {
    auto it = orders_.find(e.id);
    if (it == orders_.end()) {
        LMM_LOG(Warn, "oms") << "fill for unknown id=" << e.id;
        return;
    }
    auto& o = it->second;
    o.filled_qty += e.qty;
    o.last_update_ts = e.ts;
    update_position_on_fill(o.sym, o.side, e.px, e.qty);
    ++total_fills_;

    if (e.is_final || o.leaves() == 0) {
        o.status = OrderStatus::Filled;
    } else {
        o.status = OrderStatus::PartiallyFilled;
    }
    LMM_LOG(Debug, "oms") << "fill id=" << e.id << " px=" << e.px
                          << " qty=" << e.qty << " leaves=" << o.leaves();
}

void OMS::on_cancel_ack(const CancelAckEvent& e) {
    auto it = orders_.find(e.id);
    if (it == orders_.end()) return;
    auto& o = it->second;
    o.last_update_ts = e.ts;
    if (e.accepted) {
        // Only mark cancelled if still live (i.e. not already filled via a
        // race).
        if (!o.is_terminal()) o.status = OrderStatus::Cancelled;
    }
    // If the cancel was rejected (accepted=false), the order stays where
    // it was (likely already filled). No state change needed.
}

std::vector<const OrderRecord*> OMS::open_orders(SymbolId sym) const {
    std::vector<const OrderRecord*> out;
    for (auto& [id, o] : orders_) {
        if (o.sym == sym && !o.is_terminal()) out.push_back(&o);
    }
    return out;
}

Qty OMS::gross_open_qty(SymbolId sym) const {
    Qty q = 0;
    for (auto& [id, o] : orders_) {
        if (o.sym == sym && !o.is_terminal()) q += o.leaves();
    }
    return q;
}

void OMS::update_position_on_fill(SymbolId sym, Side side, Price px, Qty qty) {
    auto& pos = positions_[sym];
    Qty signed_qty = (side == Side::Buy ? qty : -qty);
    double fill_px = static_cast<double>(px);

    // If we are adding to the position (same sign or flat), update the
    // running VWAP. If we are reducing or flipping, realise PnL on the
    // closed portion.
    if (pos.net == 0 || (pos.net > 0 && signed_qty > 0) ||
        (pos.net < 0 && signed_qty < 0)) {
        // Same direction: blend into average.
        double new_net = static_cast<double>(pos.net + signed_qty);
        pos.avg_px_ticks = (pos.avg_px_ticks * static_cast<double>(pos.net) +
                            fill_px * static_cast<double>(signed_qty)) / new_net;
        pos.net += signed_qty;
    } else {
        // Opposing direction: close min(|pos|, |qty|) at realized PnL.
        Qty closing = std::min<Qty>(std::abs(pos.net), std::abs(signed_qty));
        double pnl_per_unit = (pos.net > 0)
            ? (fill_px - pos.avg_px_ticks)       // closing a long
            : (pos.avg_px_ticks - fill_px);      // closing a short
        pos.realized_pnl_ticks += pnl_per_unit * static_cast<double>(closing);

        const Qty before = pos.net;
        pos.net += signed_qty;
        if (pos.net == 0) {
            pos.avg_px_ticks = 0.0;
        } else if ((before > 0) != (pos.net > 0)) {
            // We flipped sides; the remaining qty was opened at fill_px.
            pos.avg_px_ticks = fill_px;
        }
        // If we only reduced (did not flip), avg_px stays the same, long or short.
    }
}

} // namespace lmm
