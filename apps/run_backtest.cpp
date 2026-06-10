// Entry point for running a backtest from the command line.
//
// Usage:
//   run_backtest <config.ini> <feed.csv>
//
// Prints a human-readable summary to stdout. Optionally writes PnL and trade
// CSVs if paths are configured in the .ini.
//
// Wiring note: the OMS and the BacktestDriver reference each other (OMS
// emits events that the driver routes to the simulator; the simulator emits
// events that the driver routes to the OMS). We break the construction
// cycle by constructing the OMS with a no-op callback, constructing the
// driver, then rebinding the OMS callback via set_send().

#include "../core/Config.hpp"
#include "../core/Logger.hpp"
#include "../core/Types.hpp"
#include "../market_data/CsvFeedReader.hpp"
#include "../orderbook/OrderBook.hpp"
#include "../execution/OMS.hpp"
#include "../risk/RiskManager.hpp"
#include "../simulator/ExchangeSimulator.hpp"
#include "../strategy/MarketMaker.hpp"
#include "../backtest/BacktestDriver.hpp"

#include <iostream>
#include <iomanip>
#include <memory>

using namespace lmm;

namespace {

LogLevel parse_level(const std::string& s) {
    if (s == "trace") return LogLevel::Trace;
    if (s == "debug") return LogLevel::Debug;
    if (s == "info")  return LogLevel::Info;
    if (s == "warn")  return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    return LogLevel::Info;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: run_backtest <config.ini> <feed.csv>\n";
        return 2;
    }
    const std::string cfg_path  = argv[1];
    const std::string feed_path = argv[2];

    Config cfg = Config::load(cfg_path);
    Logger::instance().set_level(parse_level(cfg.get_str("log_level", "info")));

    // --- Load market data -------------------------------------------------
    SymbolTable symbols;
    auto events = CsvFeedReader::read(feed_path, symbols);
    if (events.empty()) {
        std::cerr << "feed is empty\n";
        return 1;
    }
    SymbolId sym = events.front().sym;
    std::cout << "# loaded " << events.size() << " md events for symbol "
              << symbols.name(sym) << "\n";

    // --- Backtest config --------------------------------------------------
    BacktestConfig bt{};
    bt.sym = sym;
    bt.tick_size = cfg.get_double("tick_size", 0.01);
    bt.strategy_to_exchange_latency_ns =
        cfg.get_int("strategy_to_exchange_latency_ns", 100'000);
    bt.exchange_to_strategy_latency_ns =
        cfg.get_int("exchange_to_strategy_latency_ns", 100'000);
    bt.pnl_csv_path    = cfg.get_str("pnl_csv", "");
    bt.trades_csv_path = cfg.get_str("trades_csv", "");

    // --- Simulator config -------------------------------------------------
    SimulatorConfig sim_cfg{};
    sim_cfg.ack_latency_ns  = cfg.get_int("ack_latency_ns", 50'000);
    sim_cfg.fill_latency_ns = cfg.get_int("fill_latency_ns", 50'000);

    // --- Risk limits ------------------------------------------------------
    RiskLimits lim{};
    lim.max_position       = cfg.get_int("max_position",     200);
    lim.max_order_size     = cfg.get_int("max_order_size",   100);
    lim.max_open_qty       = cfg.get_int("max_open_qty",     500);
    lim.max_drawdown_ticks = cfg.get_double("max_drawdown_ticks", 1e12);

    // --- Build components with deferred wiring ---------------------------
    OMS oms([](const Event&){});  // placeholder; rebound below.
    RiskManager risk(lim, oms);

    // Raw pointer resolved post-construction; all objects live for the
    // lifetime of main() so the pointer is safe.
    BacktestDriver* driver_ptr = nullptr;

    ExchangeSimulator sim(sim_cfg,
        [&driver_ptr](Timestamp ts, Event e) {
            if (driver_ptr) driver_ptr->accept_sim_event(ts, std::move(e));
        });

    // --- Strategy ---------------------------------------------------------
    MMConfig mm{};
    mm.sym            = sym;
    mm.half_spread    = cfg.get_int("half_spread_ticks",  2);
    mm.min_spread     = cfg.get_int("min_spread_ticks",   1);
    mm.quote_size     = cfg.get_int("quote_size",        10);
    mm.max_position   = lim.max_position;
    mm.inv_skew       = cfg.get_double("inv_skew",        0.01);
    mm.requote_thresh = cfg.get_int("requote_thresh",     1);

    auto strategy = std::make_unique<MarketMaker>(mm, oms, risk);

    BacktestDriver driver(bt, std::move(strategy), oms, risk, sim);
    driver_ptr = &driver;

    oms.set_send([&driver](const Event& e) {
        driver.accept_oms_event(e);
    });

    // --- Run --------------------------------------------------------------
    auto result = driver.run(events);

    // --- Summary ----------------------------------------------------------
    double ts = bt.tick_size;
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "\n=== Backtest summary ===\n";
    std::cout << "symbol              : " << symbols.name(sym) << "\n";
    std::cout << "md events           : " << result.md_events << "\n";
    std::cout << "orders submitted    : " << result.orders_submitted << "\n";
    std::cout << "fills               : " << result.fills << "\n";
    std::cout << "ending position     : " << result.ending_position << "\n";
    std::cout << "last mid (ticks)    : " << result.last_mid << "\n";
    std::cout << "realized PnL        : " << result.realized_pnl_ticks * ts
              << "  (" << result.realized_pnl_ticks << " ticks)\n";
    std::cout << "unrealized PnL      : " << result.unrealized_pnl_ticks * ts
              << "  (" << result.unrealized_pnl_ticks << " ticks)\n";
    std::cout << "total PnL           : " << result.total_pnl_ticks * ts
              << "  (" << result.total_pnl_ticks << " ticks)\n";
    std::cout << "max drawdown        : " << result.max_drawdown_ticks * ts
              << "  (" << result.max_drawdown_ticks << " ticks)\n";
    std::cout << "kill switch tripped : "
              << (risk.killed() ? "YES" : "no") << "\n";

    return 0;
}
