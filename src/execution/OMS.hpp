#pragma once

// Order Management System.
//
// Owns the lifecycle of every order the strategy creates. Responsibilities:
//   - allocate OrderIds,
//   - track order state (New -> PendingNew -> Working -> Filled/Cancelled),
//   - handle acks, cancels, fills from the simulator,
//   - expose queries for the strategy (open orders, positions).
//
// The OMS does not talk to the exchange directly: it emits NewOrderEvents
// and CancelOrderEvents onto the bus, and the simulator consumes them and
// replies with AckEvent / FillEvent / CancelAckEvent.

#include "../core/Types.hpp"
#include "../core/Events.hpp"
#include <unordered_map>
#include <vector>
#include <functional>

namespace lmm {

struct OrderRecord {
    OrderId     id;
    SymbolId    sym;
    Side        side;
    Price       px;
    Qty         qty;          // original order qty (always positive)
    Qty         filled_qty;   // cumulative filled (always positive)
    OrderStatus status;
    Timestamp   created_ts;
    Timestamp   last_update_ts;

    Qty leaves() const { return qty - filled_qty; }
    bool is_terminal() const {
        return status == OrderStatus::Filled
            || status == OrderStatus::Cancelled
            || status == OrderStatus::Rejected;
    }
};

// Position per symbol: signed net quantity and VWAP of the open position
// for simple PnL attribution. Realized PnL is tracked separately.
struct Position {
    Qty   net = 0;           // >0 long, <0 short
    double avg_px_ticks = 0.0; // running average entry price (in ticks)
    double realized_pnl_ticks = 0.0;

    // Unrealized PnL at a given mark price (in tick units).
    double unrealized(Price mark) const {
        if (net == 0) return 0.0;
        return (static_cast<double>(mark) - avg_px_ticks) * static_cast<double>(net);
    }
};

class OMS {
public:
    // send_fn is a callback the OMS uses to emit outbound events. The
    // backtest driver wires this to the event bus, adding configurable
    // latency in the process.
    using SendFn = std::function<void(const Event&)>;

    explicit OMS(SendFn send) : send_(std::move(send)) {}

    // --- strategy-facing API ------------------------------------------------
    // Returns the new OrderId. The order starts in PendingNew.
    OrderId submit(Timestamp ts, SymbolId sym, Side side, Price px, Qty qty);

    // Returns true if a cancel was emitted. False if the order is unknown
    // or already terminal.
    bool cancel(Timestamp ts, OrderId id);

    // --- exchange-callback API ---------------------------------------------
    void on_ack(const AckEvent& e);
    void on_fill(const FillEvent& e);
    void on_cancel_ack(const CancelAckEvent& e);

    // --- queries ------------------------------------------------------------
    const OrderRecord* find(OrderId id) const {
        auto it = orders_.find(id);
        return it == orders_.end() ? nullptr : &it->second;
    }

    // All live (non-terminal) orders for a symbol.
    std::vector<const OrderRecord*> open_orders(SymbolId sym) const;

    const Position& position(SymbolId sym) const {
        static const Position zero{};
        auto it = positions_.find(sym);
        return it == positions_.end() ? zero : it->second;
    }

    // Total notional in ticks of open bids/asks for pre-trade risk checks.
    Qty gross_open_qty(SymbolId sym) const;

    std::size_t total_orders() const { return orders_.size(); }
    std::size_t total_fills() const { return total_fills_; }

private:
    void update_position_on_fill(SymbolId sym, Side side, Price px, Qty qty);

    SendFn send_;
    OrderId next_id_ = 1;
    std::unordered_map<OrderId, OrderRecord> orders_;
    std::unordered_map<SymbolId, Position>   positions_;
    std::size_t total_fills_ = 0;
};

} // namespace lmm
