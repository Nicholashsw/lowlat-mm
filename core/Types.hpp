#pragma once

// Core numeric and ID types used throughout the engine.
// Prices are represented as int64_t ticks (e.g. 1 tick = 0.0001) to avoid
// floating-point comparison issues in the order book. A Price of 12345 with
// tick_size 0.01 therefore means 123.45. Strategies and the backtest driver
// convert to/from double only at the boundaries.

#include <cstdint>
#include <chrono>
#include <string>
#include <ostream>
#include <limits>

namespace lmm {

using Price    = std::int64_t;   // price in ticks
using Qty      = std::int64_t;   // signed so we can carry net positions
using OrderId  = std::uint64_t;
using SymbolId = std::uint32_t;

// Timestamp is nanoseconds since an arbitrary epoch. In replay mode the
// "clock" is the timestamp of the current market data event, not wall time.
using Timestamp = std::int64_t;

inline constexpr Price    kInvalidPrice = std::numeric_limits<Price>::min();
inline constexpr OrderId  kInvalidOrderId = 0;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

inline const char* side_str(Side s) {
    return s == Side::Buy ? "BUY" : "SELL";
}

// Convert a tick price to a human-readable double for logging / output.
inline double to_double(Price p, double tick_size) {
    return static_cast<double>(p) * tick_size;
}

inline Price from_double(double x, double tick_size) {
    // Round-to-nearest to avoid drift on repeated conversions.
    return static_cast<Price>(x / tick_size + (x >= 0 ? 0.5 : -0.5));
}

} // namespace lmm
