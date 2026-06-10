#pragma once

// Backtest driver. Owns the event loop. Responsibilities:
//   - load market data,
//   - wire OMS -> simulator -> OMS via the event bus, adding latency,
//   - pump events in timestamp order,
//   - dispatch to the strategy, OMS, simulator,
//   - track inventory / PnL time series and produce a summary.
//
// A single backtest runs single-threaded and is fully deterministic given
// the same input file, config, and strategy.

#include "../core/EventBus.hpp"
#include "../core/Events.hpp"
#include "../core/Types.hpp"
#include "../execution/OMS.hpp"
#include "../orderbook/OrderBook.hpp"
#include "../risk/RiskManager.hpp"
#include "../simulator/ExchangeSimulator.hpp"
#include "../strategy/Strategy.hpp"
#include <string>
#include <vector>
#include <memory>

namespace lmm {

struct BacktestResult {
    std::size_t md_events        = 0;
    std::size_t orders_submitted = 0;
    std::size_t fills            = 0;
    double realized_pnl_ticks    = 0.0;
    double unrealized_pnl_ticks  = 0.0;
    double total_pnl_ticks       = 0.0;
    Qty    ending_position       = 0;
    double max_drawdown_ticks    = 0.0;
    double peak_equity_ticks     = 0.0;
    Price  last_mid              = kInvalidPrice;
};

struct BacktestConfig {
    SymbolId sym = 0;
    double   tick_size = 0.01;
    Timestamp strategy_to_exchange_latency_ns = 100'000; // 100us
    Timestamp exchange_to_strategy_latency_ns = 100'000;
    std::string pnl_csv_path;        // if non-empty, write inventory/PnL timeseries
    std::string trades_csv_path;     // if non-empty, write trade blotter
};

class BacktestDriver {
public:
    BacktestDriver(BacktestConfig cfg,
                   std::unique_ptr<Strategy> strat,
                   OMS& oms,
                   RiskManager& risk,
                   ExchangeSimulator& sim);

    // Feed an array of pre-sorted market data events and run until the
    // queue drains.
    BacktestResult run(const std::vector<TopOfBookEvent>& md);

private:
    void on_oms_outbound(const Event& e);    // OMS -> sim, with latency
    void on_sim_outbound(Timestamp ts, Event e); // sim -> OMS, with latency

    void dispatch(Timestamp ts, const Event& e);
    void record_pnl_sample(Timestamp ts);
    void write_trade(const FillEvent& f);

    BacktestConfig cfg_;
    std::unique_ptr<Strategy> strat_;
    OMS& oms_;
    RiskManager& risk_;
    ExchangeSimulator& sim_;
    EventBus bus_;
    OrderBook book_;

    BacktestResult result_{};
    std::vector<std::tuple<Timestamp, Qty, double, double, Price>> pnl_samples_;
    std::vector<std::tuple<Timestamp, OrderId, char, Price, Qty>> trade_log_;
};

} // namespace lmm
