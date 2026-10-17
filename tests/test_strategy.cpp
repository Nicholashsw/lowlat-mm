#include "catch.hpp"
#include "../strategy/MarketMaker.hpp"
#include "../execution/OMS.hpp"
#include "../risk/RiskManager.hpp"
#include "../orderbook/OrderBook.hpp"

#include <vector>

using namespace lmm;

namespace {

struct Bus {
    std::vector<Event> events;
    OMS::SendFn fn() { return [this](const Event& e) { events.push_back(e); }; }

    int count_new() const {
        int n = 0;
        for (auto& e : events) if (std::holds_alternative<NewOrderEvent>(e)) ++n;
        return n;
    }
    int count_cancel() const {
        int n = 0;
        for (auto& e : events) if (std::holds_alternative<CancelOrderEvent>(e)) ++n;
        return n;
    }
    std::vector<NewOrderEvent> news() const {
        std::vector<NewOrderEvent> out;
        for (auto& e : events)
            if (auto* p = std::get_if<NewOrderEvent>(&e)) out.push_back(*p);
        return out;
    }
    std::vector<CancelOrderEvent> cancels() const {
        std::vector<CancelOrderEvent> out;
        for (auto& e : events)
            if (auto* p = std::get_if<CancelOrderEvent>(&e)) out.push_back(*p);
        return out;
    }
};

} // namespace

TEST_CASE("strategy does not quote on crossed or one-sided book", "[strategy]") {
    Bus bus;
    OMS oms(bus.fn());
    RiskLimits lim;
    RiskManager risk(lim, oms);

    MMConfig cfg; cfg.sym = 0; cfg.half_spread = 2;
    cfg.min_spread = 1; cfg.quote_size = 5;
    MarketMaker mm(cfg, oms, risk);

    OrderBook book(0);
    // One-sided
    book.apply_level(Side::Buy, 10000, 10);
    TopOfBookEvent md{1000, 0, 10000, 10, kInvalidPrice, 0};
    mm.on_market_data(md, book);
    REQUIRE(bus.count_new() == 0);

    // Crossed
    OrderBook crossed(0);
    crossed.apply_top(10005, 10, 10003, 10);
    TopOfBookEvent md2{2000, 0, 10005, 10, 10003, 10};
    mm.on_market_data(md2, crossed);
    REQUIRE(bus.count_new() == 0);
}

TEST_CASE("strategy quotes a symmetric bid/ask at zero inventory", "[strategy]") {
    Bus bus;
    OMS oms(bus.fn());
    RiskLimits lim;
    RiskManager risk(lim, oms);

    MMConfig cfg; cfg.sym = 0; cfg.half_spread = 3;
    cfg.min_spread = 1; cfg.quote_size = 5; cfg.inv_skew = 0.0;
    MarketMaker mm(cfg, oms, risk);

    OrderBook book(0);
    book.apply_top(10000, 100, 10006, 100);   // mid = 10003, spread 6
    TopOfBookEvent md{1000, 0, 10000, 100, 10006, 100};
    mm.on_market_data(md, book);

    // Expect exactly one bid and one ask at mid +/- half_spread.
    REQUIRE(bus.count_new() == 2);
    auto news = bus.news();
    Price bid_px = 0, ask_px = 0;
    for (auto& n : news) {
        if (n.side == Side::Buy)  bid_px = n.px;
        else                      ask_px = n.px;
    }
    REQUIRE(bid_px == 10000);     // 10003 - 3
    REQUIRE(ask_px == 10006);     // 10003 + 3
    REQUIRE(ask_px > bid_px);
}

TEST_CASE("inventory skew shifts quotes in the expected direction",
          "[strategy]") {
    // With positive inventory (long), the reservation price should drop,
    // i.e. both bid and ask targets shift lower, making the ask more
    // aggressive and the bid less aggressive.
    Bus bus;
    OMS oms(bus.fn());
    RiskLimits lim;
    RiskManager risk(lim, oms);

    MMConfig cfg; cfg.sym = 0; cfg.half_spread = 3;
    cfg.min_spread = 1; cfg.quote_size = 5; cfg.inv_skew = 0.5;
    MarketMaker mm(cfg, oms, risk);

    OrderBook book(0);
    book.apply_top(10000, 100, 10006, 100);   // mid = 10003

    // Force a long position via synthetic fill.
    OrderId fid = oms.submit(10, 0, Side::Buy, 10000, 10);
    oms.on_ack(AckEvent{fid, 11, true, ""});
    oms.on_fill(FillEvent{fid, 12, 10000, 10, true});
    REQUIRE(oms.position(0).net == 10);

    bus.events.clear();  // only look at the quotes emitted next
    TopOfBookEvent md{1000, 0, 10000, 100, 10006, 100};
    mm.on_market_data(md, book);

    Price bid_px = 0, ask_px = 0;
    bool saw_new = false;
    for (auto& e : bus.events) {
        if (auto* n = std::get_if<NewOrderEvent>(&e)) {
            saw_new = true;
            if (n->side == Side::Buy)  bid_px = n->px;
            else                       ask_px = n->px;
        }
    }
    REQUIRE(saw_new);
    // inv_skew * pos = 0.5 * 10 = 5 ticks shift down
    // reservation = 10003 - 5 = 9998; bid = 9995, ask = 10001
    REQUIRE(bid_px == 9995);
    REQUIRE(ask_px == 10001);
}

TEST_CASE("strategy stops quoting a side at max_position", "[strategy]") {
    Bus bus;
    OMS oms(bus.fn());
    RiskLimits lim;
    RiskManager risk(lim, oms);

    MMConfig cfg; cfg.sym = 0; cfg.half_spread = 3; cfg.min_spread = 1;
    cfg.quote_size = 5; cfg.max_position = 10; cfg.inv_skew = 0.0;
    MarketMaker mm(cfg, oms, risk);

    OrderBook book(0);
    book.apply_top(10000, 100, 10006, 100);

    // Push net long to +10 so buy side is at the cap.
    OrderId fid = oms.submit(10, 0, Side::Buy, 10000, 10);
    oms.on_ack(AckEvent{fid, 11, true, ""});
    oms.on_fill(FillEvent{fid, 12, 10000, 10, true});

    bus.events.clear();
    TopOfBookEvent md{1000, 0, 10000, 100, 10006, 100};
    mm.on_market_data(md, book);

    // Only the ask should be quoted.
    int bids = 0, asks = 0;
    for (auto& e : bus.events) {
        if (auto* n = std::get_if<NewOrderEvent>(&e)) {
            if (n->side == Side::Buy) ++bids; else ++asks;
        }
    }
    REQUIRE(bids == 0);
    REQUIRE(asks == 1);
}

TEST_CASE("cancel_all stamps cancels with the triggering event time",
          "[strategy][regression]") {
    // Regression: cancel_all() used to hardcode a timestamp of 0. The driver
    // schedules a cancel at ts + strategy_to_exchange_latency, so a zero here
    // produced an arrival time in the *past* relative to the simulation clock,
    // which on a min-heap event bus pops immediately and out of chronological
    // order. Every cancel must carry the timestamp of the market-data event
    // that triggered it.
    Bus bus;
    OMS oms(bus.fn());
    RiskLimits lim;
    RiskManager risk(lim, oms);

    MMConfig cfg; cfg.sym = 0; cfg.half_spread = 3; cfg.min_spread = 1;
    cfg.quote_size = 5; cfg.inv_skew = 0.0;
    MarketMaker mm(cfg, oms, risk);

    constexpr Timestamp kQuoteTs  = 5'000'000'000;
    constexpr Timestamp kCancelTs = 6'000'000'000;

    // 1. Quote normally so that both sides are live.
    OrderBook book(0);
    book.apply_top(10000, 100, 10006, 100);
    TopOfBookEvent md{kQuoteTs, 0, 10000, 100, 10006, 100};
    mm.on_market_data(md, book);
    REQUIRE(bus.count_new() == 2);

    // 2. Crossed book -> cancel_all() path.
    bus.events.clear();
    OrderBook crossed(0);
    crossed.apply_top(10005, 10, 10003, 10);
    TopOfBookEvent md2{kCancelTs, 0, 10005, 10, 10003, 10};
    mm.on_market_data(md2, crossed);

    auto cans = bus.cancels();
    REQUIRE(cans.size() == 2);
    for (const auto& c : cans) {
        REQUIRE(c.ts == kCancelTs);   // was 0 before the fix
        REQUIRE(c.ts != 0);
    }
}

TEST_CASE("kill switch cancels carry the triggering event time",
          "[strategy][regression]") {
    // The kill-switch branch is a second route into cancel_all().
    Bus bus;
    OMS oms(bus.fn());
    RiskLimits lim;
    RiskManager risk(lim, oms);

    MMConfig cfg; cfg.sym = 0; cfg.half_spread = 3; cfg.min_spread = 1;
    cfg.quote_size = 5; cfg.inv_skew = 0.0;
    MarketMaker mm(cfg, oms, risk);

    constexpr Timestamp kQuoteTs = 7'000'000'000;
    constexpr Timestamp kKillTs  = 8'000'000'000;

    OrderBook book(0);
    book.apply_top(10000, 100, 10006, 100);
    mm.on_market_data(TopOfBookEvent{kQuoteTs, 0, 10000, 100, 10006, 100}, book);
    REQUIRE(bus.count_new() == 2);

    bus.events.clear();
    risk.kill("test");
    mm.on_market_data(TopOfBookEvent{kKillTs, 0, 10000, 100, 10006, 100}, book);

    auto cans = bus.cancels();
    REQUIRE(cans.size() == 2);
    for (const auto& c : cans) REQUIRE(c.ts == kKillTs);
}
