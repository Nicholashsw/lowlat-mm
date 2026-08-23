#include "catch.hpp"
#include "../simulator/ExchangeSimulator.hpp"
#include <vector>

using namespace lmm;

namespace {

struct Capture {
    std::vector<std::pair<Timestamp, Event>> out;
    ExchangeSimulator::EmitFn fn() {
        return [this](Timestamp t, Event e) { out.emplace_back(t, std::move(e)); };
    }

    template <typename T>
    const T* find() const {
        for (auto& [t, e] : out) {
            if (auto* p = std::get_if<T>(&e)) return p;
        }
        return nullptr;
    }
};

} // namespace

TEST_CASE("new order emits ack after ack_latency", "[sim]") {
    SimulatorConfig cfg; cfg.ack_latency_ns = 100;
    Capture cap;
    ExchangeSimulator sim(cfg, cap.fn());

    NewOrderEvent n{1, 1000, 0, Side::Buy, 10000, 10};
    sim.on_new_order(n);
    REQUIRE(cap.out.size() == 1);
    auto* ack = cap.find<AckEvent>();
    REQUIRE(ack != nullptr);
    REQUIRE(ack->id == 1);
    REQUIRE(ack->accepted);
    REQUIRE(ack->ts == 1100);
}

TEST_CASE("marketable buy fills immediately at best_ask", "[sim]") {
    SimulatorConfig cfg; cfg.ack_latency_ns = 10; cfg.fill_latency_ns = 20;
    Capture cap;
    ExchangeSimulator sim(cfg, cap.fn());

    // Seed book: bid 9998, ask 10000.
    sim.on_market_data(TopOfBookEvent{500, 0, 9998, 100, 10000, 100});

    // Buy @ 10001 -> marketable, should fill at 10000.
    NewOrderEvent n{7, 1000, 0, Side::Buy, 10001, 5};
    sim.on_new_order(n);

    auto* fill = cap.find<FillEvent>();
    REQUIRE(fill != nullptr);
    REQUIRE(fill->id == 7);
    REQUIRE(fill->px == 10000);
    REQUIRE(fill->qty == 5);
    REQUIRE(fill->is_final);
}

TEST_CASE("strict trade-through rule (fill_on_touch=false)", "[sim]") {
    SimulatorConfig cfg;
    cfg.ack_latency_ns = 10; cfg.fill_latency_ns = 20;
    cfg.fill_on_touch = false;
    Capture cap;
    ExchangeSimulator sim(cfg, cap.fn());

    sim.on_market_data(TopOfBookEvent{500, 0, 9998, 100, 10000, 100});

    NewOrderEvent n{1, 1000, 0, Side::Buy, 9999, 5};
    sim.on_new_order(n);

    // Ask drops TO our bid: under strict rule, no fill.
    sim.on_market_data(TopOfBookEvent{2000, 0, 9997, 100, 9999, 100});
    REQUIRE(cap.find<FillEvent>() == nullptr);

    // Ask drops BELOW our bid: market traded through, fill.
    sim.on_market_data(TopOfBookEvent{3000, 0, 9996, 100, 9998, 100});
    auto* fill = cap.find<FillEvent>();
    REQUIRE(fill != nullptr);
    REQUIRE(fill->id == 1);
    REQUIRE(fill->px == 9999);
    REQUIRE(fill->qty == 5);
}

TEST_CASE("fill-on-touch rule (default) fills when market reaches our price",
          "[sim]") {
    SimulatorConfig cfg;
    cfg.ack_latency_ns = 10; cfg.fill_latency_ns = 20;
    // fill_on_touch defaults to true.
    Capture cap;
    ExchangeSimulator sim(cfg, cap.fn());

    sim.on_market_data(TopOfBookEvent{500, 0, 9998, 100, 10000, 100});

    NewOrderEvent n{1, 1000, 0, Side::Buy, 9999, 5};
    sim.on_new_order(n);

    // Ask drops to our bid: fills under fill-on-touch.
    sim.on_market_data(TopOfBookEvent{2000, 0, 9997, 100, 9999, 100});
    auto* fill = cap.find<FillEvent>();
    REQUIRE(fill != nullptr);
    REQUIRE(fill->id == 1);
    REQUIRE(fill->px == 9999);
}

TEST_CASE("cancel before fill acks accepted; cancel after fill acks rejected",
          "[sim]") {
    SimulatorConfig cfg; cfg.ack_latency_ns = 10; cfg.fill_latency_ns = 10;
    Capture cap;
    ExchangeSimulator sim(cfg, cap.fn());

    sim.on_market_data(TopOfBookEvent{500, 0, 9998, 10, 10000, 10});
    NewOrderEvent n{42, 1000, 0, Side::Buy, 9999, 5};
    sim.on_new_order(n);

    // Cancel while resting -> accepted.
    sim.on_cancel_order(CancelOrderEvent{42, 1500});
    bool saw_accepted_cancel = false;
    for (auto& [t, e] : cap.out) {
        if (auto* c = std::get_if<CancelAckEvent>(&e)) {
            if (c->id == 42 && c->accepted) saw_accepted_cancel = true;
        }
    }
    REQUIRE(saw_accepted_cancel);

    // Second cancel for same id -> not resting anymore, accepted=false.
    sim.on_cancel_order(CancelOrderEvent{42, 2000});
    int reject_count = 0;
    for (auto& [t, e] : cap.out) {
        if (auto* c = std::get_if<CancelAckEvent>(&e)) {
            if (c->id == 42 && !c->accepted) ++reject_count;
        }
    }
    REQUIRE(reject_count == 1);
}
