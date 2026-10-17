#include "catch.hpp"
#include "../orderbook/OrderBook.hpp"

using namespace lmm;

TEST_CASE("empty book has no bid or ask", "[orderbook]") {
    OrderBook b;
    REQUIRE_FALSE(b.has_bid());
    REQUIRE_FALSE(b.has_ask());
    REQUIRE_FALSE(b.two_sided());
    REQUIRE(b.best_bid() == kInvalidPrice);
    REQUIRE(b.best_ask() == kInvalidPrice);
    REQUIRE(b.mid()     == kInvalidPrice);
    REQUIRE(b.spread()  == kInvalidPrice);
}

TEST_CASE("apply_top sets best bid/ask and computes mid/spread", "[orderbook]") {
    OrderBook b;
    b.apply_top(/*bid*/ 10000, 100, /*ask*/ 10004, 150);
    REQUIRE(b.best_bid() == 10000);
    REQUIRE(b.best_ask() == 10004);
    REQUIRE(b.best_bid_size() == 100);
    REQUIRE(b.best_ask_size() == 150);
    REQUIRE(b.spread() == 4);
    REQUIRE(b.mid()    == 10002);
    REQUIRE_FALSE(b.crossed());
}

TEST_CASE("apply_top replaces previous state (no stale depth leaks)", "[orderbook]") {
    OrderBook b;
    b.apply_top(10000, 100, 10004, 100);
    b.apply_top(10010, 50, 10012, 75);
    REQUIRE(b.best_bid() == 10010);
    REQUIRE(b.best_ask() == 10012);
    REQUIRE(b.best_bid_size() == 50);
    REQUIRE(b.best_ask_size() == 75);
}

TEST_CASE("apply_level builds depth and removes on size=0", "[orderbook]") {
    OrderBook b;
    b.apply_level(Side::Buy,  9998, 10);
    b.apply_level(Side::Buy,  9999, 20);
    b.apply_level(Side::Buy, 10000, 30);
    b.apply_level(Side::Sell, 10002, 15);
    b.apply_level(Side::Sell, 10003, 25);

    REQUIRE(b.best_bid() == 10000);
    REQUIRE(b.best_bid_size() == 30);
    REQUIRE(b.best_ask() == 10002);

    b.apply_level(Side::Buy, 10000, 0);  // remove top bid
    REQUIRE(b.best_bid() == 9999);
    REQUIRE(b.best_bid_size() == 20);
}

TEST_CASE("crossed book detected", "[orderbook]") {
    OrderBook b;
    b.apply_top(10005, 10, 10003, 10);  // bid above ask
    REQUIRE(b.two_sided());
    REQUIRE(b.crossed());
    REQUIRE(b.mid() == kInvalidPrice); // mid undefined when crossed
}

TEST_CASE("one-sided book returns invalid mid/spread", "[orderbook]") {
    OrderBook b;
    b.apply_level(Side::Buy, 10000, 10);
    REQUIRE(b.has_bid());
    REQUIRE_FALSE(b.has_ask());
    REQUIRE(b.mid()    == kInvalidPrice);
    REQUIRE(b.spread() == kInvalidPrice);
}
