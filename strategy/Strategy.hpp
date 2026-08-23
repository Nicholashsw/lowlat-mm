#pragma once

// Strategy base class. A strategy sees market data updates and its own
// order lifecycle events; it decides what orders to send via the OMS.
//
// The interface is deliberately small: on_market_data() is the primary hook,
// and the rest are notifications for bookkeeping. Strategies should not
// hold raw state that the OMS already tracks (positions, open orders);
// they should query the OMS instead.

#include "../core/Events.hpp"
#include "../execution/OMS.hpp"
#include "../orderbook/OrderBook.hpp"
#include "../risk/RiskManager.hpp"

namespace lmm {

class Strategy {
public:
    virtual ~Strategy() = default;

    virtual void on_market_data(const TopOfBookEvent& md,
                                const OrderBook& book) = 0;

    virtual void on_ack(const AckEvent&) {}
    virtual void on_fill(const FillEvent&) {}
    virtual void on_cancel_ack(const CancelAckEvent&) {}

    virtual const char* name() const = 0;
};

} // namespace lmm
