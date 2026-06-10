#pragma once

// Event types flowing through the engine. We use a tagged std::variant
// rather than a polymorphic hierarchy because:
//  - the set of event types is closed and known at compile time,
//  - no heap allocation per event,
//  - visitors make the handling code explicit.
//
// This is a common pattern in modern event-driven C++ and keeps the hot path
// cache-friendly while staying readable.

#include "Types.hpp"
#include <variant>
#include <string>

namespace lmm {

// --- Market data -----------------------------------------------------------

// Top-of-book update: the simplest useful feed shape. A real L2 feed would
// carry incremental level updates; we can add that later without touching
// the event bus.
struct TopOfBookEvent {
    Timestamp ts;
    SymbolId  sym;
    Price     bid_px;
    Qty       bid_sz;
    Price     ask_px;
    Qty       ask_sz;
};

// Public trade print. Not strictly required for the toy strategy but useful
// for realism and for strategies that react to prints.
struct TradeEvent {
    Timestamp ts;
    SymbolId  sym;
    Price     px;
    Qty       sz;
    Side      aggressor;
};

// --- OMS / exchange --------------------------------------------------------

enum class OrderStatus : std::uint8_t {
    New,                 // created client-side, not yet sent
    PendingNew,          // sent to exchange, awaiting ack
    Working,             // live on book
    PendingCancel,       // cancel sent, awaiting ack
    Cancelled,
    PartiallyFilled,
    Filled,
    Rejected
};

const char* to_string(OrderStatus s);

struct NewOrderEvent {
    OrderId   id;
    Timestamp ts;        // strategy send time (sim time)
    SymbolId  sym;
    Side      side;
    Price     px;
    Qty       qty;
};

struct CancelOrderEvent {
    OrderId   id;
    Timestamp ts;
};

struct AckEvent {
    OrderId   id;
    Timestamp ts;
    bool      accepted;    // false => Rejected (e.g. risk breach exchange-side)
    std::string reason;    // empty on accept
};

struct FillEvent {
    OrderId   id;
    Timestamp ts;
    Price     px;
    Qty       qty;         // always positive; side comes from the parent order
    bool      is_final;    // true if this fill completes the order
};

struct CancelAckEvent {
    OrderId   id;
    Timestamp ts;
    bool      accepted;    // false => too late, already filled
};

// --- The event variant -----------------------------------------------------

using Event = std::variant<
    TopOfBookEvent,
    TradeEvent,
    NewOrderEvent,
    CancelOrderEvent,
    AckEvent,
    FillEvent,
    CancelAckEvent
>;

} // namespace lmm
