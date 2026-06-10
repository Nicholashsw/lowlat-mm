#include "catch.hpp"
#include "../execution/OMS.hpp"
#include "../risk/RiskManager.hpp"

using namespace lmm;

TEST_CASE("max_order_size rejects oversize", "[risk]") {
    OMS oms([](const Event&){});
    RiskLimits lim; lim.max_order_size = 50;
    RiskManager r(lim, oms);

    std::string reason;
    REQUIRE(r.check_new_order(0, Side::Buy,  50, reason));
    REQUIRE_FALSE(r.check_new_order(0, Side::Buy, 51, reason));
    REQUIRE(reason == "order_size");
}

TEST_CASE("max_position projected limit catches over-extension", "[risk]") {
    OMS oms([](const Event&){});
    RiskLimits lim;
    lim.max_position = 20;
    lim.max_order_size = 100;
    lim.max_open_qty   = 100;
    RiskManager r(lim, oms);

    // Submit a buy and assume filled by injecting on_fill; then try another
    // buy whose projection would blow past 20.
    OrderId id = oms.submit(10, 0, Side::Buy, 100, 15);
    oms.on_ack(AckEvent{id, 11, true, ""});
    oms.on_fill(FillEvent{id, 12, 100, 15, true});
    REQUIRE(oms.position(0).net == 15);

    std::string reason;
    REQUIRE(r.check_new_order(0, Side::Buy, 10, reason) == false);
    REQUIRE(reason == "max_position");

    // Selling is fine — brings us back toward flat.
    REQUIRE(r.check_new_order(0, Side::Sell, 10, reason));
}

TEST_CASE("max_open_qty caps outstanding leaves", "[risk]") {
    OMS oms([](const Event&){});
    RiskLimits lim;
    lim.max_position   = 1000;
    lim.max_order_size = 100;
    lim.max_open_qty   = 30;
    RiskManager r(lim, oms);

    // Park two live orders totalling 30.
    oms.submit(10, 0, Side::Buy, 100, 15);
    oms.submit(10, 0, Side::Sell, 105, 15);

    std::string reason;
    REQUIRE_FALSE(r.check_new_order(0, Side::Buy, 1, reason));
    REQUIRE(reason == "max_open_qty");
}

TEST_CASE("kill switch blocks all new orders until reset", "[risk]") {
    OMS oms([](const Event&){});
    RiskLimits lim;
    RiskManager r(lim, oms);

    r.kill("manual");
    std::string reason;
    REQUIRE_FALSE(r.check_new_order(0, Side::Buy, 1, reason));
    REQUIRE(reason == "kill_switch");
    REQUIRE(r.killed());

    r.reset();
    REQUIRE(r.check_new_order(0, Side::Buy, 1, reason));
}

TEST_CASE("drawdown trips kill switch", "[risk]") {
    OMS oms([](const Event&){});
    RiskLimits lim; lim.max_drawdown_ticks = 5.0;
    RiskManager r(lim, oms);

    r.mark_equity(10.0);   // peak = 10
    r.mark_equity(8.0);    // dd = 2, ok
    REQUIRE_FALSE(r.killed());
    r.mark_equity(3.0);    // dd = 7, exceeds 5
    REQUIRE(r.killed());
}
