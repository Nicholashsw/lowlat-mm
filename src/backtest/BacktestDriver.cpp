#include "BacktestDriver.hpp"
#include "../core/Logger.hpp"
#include <fstream>
#include <algorithm>
#include <variant>

namespace lmm {

BacktestDriver::BacktestDriver(BacktestConfig cfg,
                               std::unique_ptr<Strategy> strat,
                               OMS& oms,
                               RiskManager& risk,
                               ExchangeSimulator& sim)
    : cfg_(cfg), strat_(std::move(strat)), oms_(oms), risk_(risk), sim_(sim),
      book_(cfg.sym) {}

void BacktestDriver::on_oms_outbound(const Event& e) {
    // Called synchronously when the strategy submits/cancels. We schedule
    // arrival at the simulator after strategy_to_exchange_latency.
    std::visit([this](auto&& ev) {
        using T = std::decay_t<decltype(ev)>;
        Timestamp arrival = ev.ts + cfg_.strategy_to_exchange_latency_ns;
        if constexpr (std::is_same_v<T, NewOrderEvent>) {
            auto e2 = ev; e2.ts = arrival;
            bus_.push(arrival, Event{e2});
        } else if constexpr (std::is_same_v<T, CancelOrderEvent>) {
            auto e2 = ev; e2.ts = arrival;
            bus_.push(arrival, Event{e2});
        }
    }, e);
}

void BacktestDriver::on_sim_outbound(Timestamp ts, Event e) {
    // Simulator emits at ts; OMS sees it after exchange_to_strategy_latency.
    Timestamp arrival = ts + cfg_.exchange_to_strategy_latency_ns;
    std::visit([&](auto& ev) { ev.ts = arrival; }, e);
    bus_.push(arrival, std::move(e));
}

BacktestResult BacktestDriver::run(const std::vector<TopOfBookEvent>& md) {
    // Seed the bus with market data events.
    for (const auto& m : md) {
        bus_.push(m.ts, Event{m});
    }
    result_.md_events = md.size();

    while (!bus_.empty()) {
        auto [ts, e] = bus_.pop();
        Logger::instance().set_sim_time(ts);
        dispatch(ts, e);
    }

    // Final accounting.
    const auto& pos = oms_.position(cfg_.sym);
    result_.realized_pnl_ticks = pos.realized_pnl_ticks;
    result_.ending_position = pos.net;
    result_.fills = oms_.total_fills();
    result_.orders_submitted = oms_.total_orders();
    result_.last_mid = book_.two_sided() ? book_.mid() : kInvalidPrice;
    if (result_.last_mid != kInvalidPrice) {
        result_.unrealized_pnl_ticks = pos.unrealized(result_.last_mid);
    }
    result_.total_pnl_ticks = result_.realized_pnl_ticks +
                              result_.unrealized_pnl_ticks;
    result_.peak_equity_ticks = risk_.peak_equity_ticks();

    // Compute max drawdown from the sampled equity path.
    double peak = 0.0, mdd = 0.0;
    bool seeded = false;
    for (const auto& [t, q, r, u, m] : pnl_samples_) {
        double eq = r + u;
        if (!seeded) { peak = eq; seeded = true; }
        if (eq > peak) peak = eq;
        double dd = peak - eq;
        if (dd > mdd) mdd = dd;
    }
    result_.max_drawdown_ticks = mdd;

    // Write optional CSV outputs.
    if (!cfg_.pnl_csv_path.empty()) {
        std::ofstream out(cfg_.pnl_csv_path);
        out << "ts_ns,net_position,realized_pnl_ticks,unrealized_pnl_ticks,mid_ticks\n";
        for (const auto& [t, q, r, u, m] : pnl_samples_) {
            out << t << "," << q << "," << r << "," << u << ","
                << (m == kInvalidPrice ? std::string("") : std::to_string(m))
                << "\n";
        }
    }
    if (!cfg_.trades_csv_path.empty()) {
        std::ofstream out(cfg_.trades_csv_path);
        out << "ts_ns,order_id,side,px_ticks,qty\n";
        for (const auto& [t, id, s, p, q] : trade_log_) {
            out << t << "," << id << "," << s << "," << p << "," << q << "\n";
        }
    }

    return result_;
}

void BacktestDriver::dispatch(Timestamp ts, const Event& e) {
    std::visit([&, ts](auto&& ev) {
        using T = std::decay_t<decltype(ev)>;

        if constexpr (std::is_same_v<T, TopOfBookEvent>) {
            book_.apply_top(ev.bid_px, ev.bid_sz, ev.ask_px, ev.ask_sz);
            sim_.on_market_data(ev);
            strat_->on_market_data(ev, book_);
            record_pnl_sample(ts);

        } else if constexpr (std::is_same_v<T, NewOrderEvent>) {
            sim_.on_new_order(ev);

        } else if constexpr (std::is_same_v<T, CancelOrderEvent>) {
            sim_.on_cancel_order(ev);

        } else if constexpr (std::is_same_v<T, AckEvent>) {
            oms_.on_ack(ev);
            strat_->on_ack(ev);

        } else if constexpr (std::is_same_v<T, FillEvent>) {
            oms_.on_fill(ev);
            strat_->on_fill(ev);
            write_trade(ev);
            record_pnl_sample(ts);

        } else if constexpr (std::is_same_v<T, CancelAckEvent>) {
            oms_.on_cancel_ack(ev);
            strat_->on_cancel_ack(ev);

        } else if constexpr (std::is_same_v<T, TradeEvent>) {
            // Not used in the basic backtest, but dispatched for
            // completeness so future strategies can consume prints.
            (void)ev;
        }
    }, e);
}

void BacktestDriver::record_pnl_sample(Timestamp ts) {
    const auto& pos = oms_.position(cfg_.sym);
    Price mid = book_.two_sided() ? book_.mid() : kInvalidPrice;
    double urp = (mid == kInvalidPrice) ? 0.0 : pos.unrealized(mid);
    pnl_samples_.emplace_back(ts, pos.net, pos.realized_pnl_ticks, urp, mid);
    risk_.mark_equity(pos.realized_pnl_ticks + urp);
}

void BacktestDriver::write_trade(const FillEvent& f) {
    const auto* o = oms_.find(f.id);
    if (!o) return;
    char s = (o->side == Side::Buy) ? 'B' : 'S';
    trade_log_.emplace_back(f.ts, f.id, s, f.px, f.qty);
}

// --- Static helper to wire OMS send callback to driver ---------------------
// (Used in apps/run_backtest.cpp to wire via lambda.)

} // namespace lmm
