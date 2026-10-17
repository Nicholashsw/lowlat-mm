#include "catch.hpp"
#include "../execution/OMS.hpp"
#include <vector>

using namespace lmm;

namespace {

// Captures every outbound event for assertions.
struct Recorder {
    std::vector<Event> events;
    OMS::SendFn fn() { return [this](const Event& e) { events.push_back(e); }; }
};

} // namespace

TEST_CASE("submit emits NewOrder and tracks PendingNew state", "[oms]") {
    Recorder rec;
    OMS oms(rec.fn());
    OrderId id = oms.submit(100, /*sym*/ 0, Side::Buy, 10000, 10);

    REQUIRE(id != kInvalidOrderId);
    REQUIRE(rec.events.size() == 1);
    REQUIRE(std::holds_alternative<NewOrderEvent>(rec.events[0]));

    const auto* o = oms.find(id);
    REQUIRE(o != nullptr);
    REQUIRE(o->status == OrderStatus::PendingNew);
    REQUIRE(o->qty == 10);
    REQUIRE(o->filled_qty == 0);
    REQUIRE(o->leaves() == 10);
}

TEST_CASE("ack transitions PendingNew -> Working", "[oms]") {
    Recorder rec;
    OMS oms(rec.fn());
    OrderId id = oms.submit(100, 0, Side::Buy, 10000, 10);
    oms.on_ack(AckEvent{id, 200, true, ""});
    REQUIRE(oms.find(id)->status == OrderStatus::Working);
}

TEST_CASE("reject transitions to Rejected (terminal)", "[oms]") {
    Recorder rec;
    OMS oms(rec.fn());
    OrderId id = oms.submit(100, 0, Side::Buy, 10000, 10);
    oms.on_ack(AckEvent{id, 200, false, "test"});
    const auto* o = oms.find(id);
    REQUIRE(o->status == OrderStatus::Rejected);
    REQUIRE(o->is_terminal());
}

TEST_CASE("partial fill keeps order Working-equivalent, final fill completes",
          "[oms]") {
    Recorder rec;
    OMS oms(rec.fn());
    OrderId id = oms.submit(100, 0, Side::Buy, 10000, 10);
    oms.on_ack(AckEvent{id, 150, true, ""});
    oms.on_fill(FillEvent{id, 200, 10000, 4, false});
    REQUIRE(oms.find(id)->status == OrderStatus::PartiallyFilled);
    REQUIRE(oms.find(id)->filled_qty == 4);
    REQUIRE(oms.find(id)->leaves() == 6);

    oms.on_fill(FillEvent{id, 300, 10000, 6, true});
    REQUIRE(oms.find(id)->status == OrderStatus::Filled);
    REQUIRE(oms.find(id)->is_terminal());
    REQUIRE(oms.total_fills() == 2);
}

TEST_CASE("cancel emits CancelOrder and transitions to PendingCancel", "[oms]") {
    Recorder rec;
    OMS oms(rec.fn());
    OrderId id = oms.submit(100, 0, Side::Buy, 10000, 10);
    oms.on_ack(AckEvent{id, 150, true, ""});
    REQUIRE(oms.cancel(200, id));
    REQUIRE(oms.find(id)->status == OrderStatus::PendingCancel);
    REQUIRE(rec.events.size() == 2);
    REQUIRE(std::holds_alternative<CancelOrderEvent>(rec.events.back()));

    oms.on_cancel_ack(CancelAckEvent{id, 250, true});
    REQUIRE(oms.find(id)->status == OrderStatus::Cancelled);
}

TEST_CASE("cancel on unknown or terminal order returns false", "[oms]") {
    Recorder rec;
    OMS oms(rec.fn());
    REQUIRE_FALSE(oms.cancel(100, /*unknown*/ 999));

    OrderId id = oms.submit(100, 0, Side::Buy, 10000, 10);
    oms.on_ack(AckEvent{id, 150, false, "x"});  // Rejected -> terminal
    REQUIRE_FALSE(oms.cancel(200, id));
}

TEST_CASE("position updates: long then reduce realises PnL", "[oms]") {
    Recorder rec;
    OMS oms(rec.fn());

    // Buy 10 @ 100
    OrderId b1 = oms.submit(10, 0, Side::Buy, 100, 10);
    oms.on_ack(AckEvent{b1, 11, true, ""});
    oms.on_fill(FillEvent{b1, 12, 100, 10, true});
    REQUIRE(oms.position(0).net == 10);
    REQUIRE(oms.position(0).avg_px_ticks == Approx(100.0));
    REQUIRE(oms.position(0).realized_pnl_ticks == Approx(0.0));

    // Sell 6 @ 105 -> realise (105-100)*6 = 30
    OrderId s1 = oms.submit(20, 0, Side::Sell, 105, 6);
    oms.on_ack(AckEvent{s1, 21, true, ""});
    oms.on_fill(FillEvent{s1, 22, 105, 6, true});
    REQUIRE(oms.position(0).net == 4);
    REQUIRE(oms.position(0).realized_pnl_ticks == Approx(30.0));
}

TEST_CASE("position updates: covering part of a short keeps its entry price", "[oms]") {
    Recorder rec;
    OMS oms(rec.fn());

    // Sell 20 @ 100, then buy back 10 @ 90 (+100) and 10 @ 95 (+50).
    OrderId s1 = oms.submit(10, 0, Side::Sell, 100, 20);
    oms.on_fill(FillEvent{s1, 12, 100, 20, true});
    OrderId b1 = oms.submit(20, 0, Side::Buy, 90, 10);
    oms.on_fill(FillEvent{b1, 22, 90, 10, true});
    REQUIRE(oms.position(0).net == -10);
    REQUIRE(oms.position(0).avg_px_ticks == Approx(100.0));
    REQUIRE(oms.position(0).realized_pnl_ticks == Approx(100.0));
    REQUIRE(oms.position(0).unrealized(98) == Approx(20.0));

    OrderId b2 = oms.submit(30, 0, Side::Buy, 95, 10);
    oms.on_fill(FillEvent{b2, 32, 95, 10, true});
    REQUIRE(oms.position(0).net == 0);
    REQUIRE(oms.position(0).realized_pnl_ticks == Approx(150.0));
}

TEST_CASE("position updates: a flip opens the remainder at the fill price", "[oms]") {
    Recorder rec;
    OMS oms(rec.fn());

    OrderId b1 = oms.submit(10, 0, Side::Buy, 100, 10);
    oms.on_fill(FillEvent{b1, 12, 100, 10, true});
    OrderId s1 = oms.submit(20, 0, Side::Sell, 104, 25);   // close 10 (+40), open 15 short
    oms.on_fill(FillEvent{s1, 22, 104, 25, true});
    REQUIRE(oms.position(0).net == -15);
    REQUIRE(oms.position(0).avg_px_ticks == Approx(104.0));
    REQUIRE(oms.position(0).realized_pnl_ticks == Approx(40.0));

    OrderId b2 = oms.submit(30, 0, Side::Buy, 101, 20);    // close 15 (+45), open 5 long
    oms.on_fill(FillEvent{b2, 32, 101, 20, true});
    REQUIRE(oms.position(0).net == 5);
    REQUIRE(oms.position(0).avg_px_ticks == Approx(101.0));
    REQUIRE(oms.position(0).realized_pnl_ticks == Approx(85.0));
}

TEST_CASE("gross_open_qty sums leaves per symbol", "[oms]") {
    Recorder rec;
    OMS oms(rec.fn());
    OrderId a = oms.submit(1, 0, Side::Buy, 100, 10);
    OrderId b = oms.submit(1, 0, Side::Sell, 105, 15);
    OrderId c = oms.submit(1, 1, Side::Buy, 50, 20);  // different symbol

    REQUIRE(oms.gross_open_qty(0) == 25);
    REQUIRE(oms.gross_open_qty(1) == 20);

    // Ack + partial fill should reduce leaves.
    oms.on_ack(AckEvent{a, 2, true, ""});
    oms.on_fill(FillEvent{a, 3, 100, 4, false});
    REQUIRE(oms.gross_open_qty(0) == 21);

    (void)b; (void)c;
}
