#include "MarketMaker.hpp"
#include "../core/Logger.hpp"
#include <cmath>
#include <algorithm>
#include <cstdlib>

namespace lmm {

void MarketMaker::on_market_data(const TopOfBookEvent& md,
                                 const OrderBook& book) {
    if (md.sym != cfg_.sym) return;

    if (risk_.killed()) { cancel_all(); return; }

    if (!book.two_sided() || book.crossed()) {
        cancel_all();
        return;
    }

    Price spread = book.spread();
    if (spread < cfg_.min_spread) {
        // Market tighter than we're willing to quote: stand down.
        cancel_all();
        return;
    }

    Price mid = book.mid();
    const auto& pos = oms_.position(cfg_.sym);
    // Inventory skew: r = mid - k * position (in ticks).
    double skewed = static_cast<double>(mid) - cfg_.inv_skew * static_cast<double>(pos.net);

    Price target_bid = static_cast<Price>(std::floor(skewed - cfg_.half_spread));
    Price target_ask = static_cast<Price>(std::ceil (skewed + cfg_.half_spread));

    // Stay inside the visible book (don't cross).
    target_bid = std::min(target_bid, book.best_ask() - 1);
    target_ask = std::max(target_ask, book.best_bid() + 1);

    // Sanity: keep bid < ask.
    if (target_bid >= target_ask) {
        cancel_all();
        return;
    }

    // Skip quoting on the side that would blow past max_position.
    bool quote_bid = (pos.net + cfg_.quote_size) <=  cfg_.max_position;
    bool quote_ask = (pos.net - cfg_.quote_size) >= -cfg_.max_position;

    // BID side -----------------------------------------------------------
    if (quote_bid) {
        bool need_new = (live_bid_id_ == 0) ||
                        (std::abs(live_bid_px_ - target_bid) >= cfg_.requote_thresh);
        if (need_new) {
            if (live_bid_id_ != 0) {
                oms_.cancel(md.ts, live_bid_id_);
                live_bid_id_ = 0;
                live_bid_px_ = kInvalidPrice;
            }
            try_submit(md.ts, Side::Buy, target_bid);
        }
    } else if (live_bid_id_ != 0) {
        oms_.cancel(md.ts, live_bid_id_);
        live_bid_id_ = 0;
        live_bid_px_ = kInvalidPrice;
    }

    // ASK side -----------------------------------------------------------
    if (quote_ask) {
        bool need_new = (live_ask_id_ == 0) ||
                        (std::abs(live_ask_px_ - target_ask) >= cfg_.requote_thresh);
        if (need_new) {
            if (live_ask_id_ != 0) {
                oms_.cancel(md.ts, live_ask_id_);
                live_ask_id_ = 0;
                live_ask_px_ = kInvalidPrice;
            }
            try_submit(md.ts, Side::Sell, target_ask);
        }
    } else if (live_ask_id_ != 0) {
        oms_.cancel(md.ts, live_ask_id_);
        live_ask_id_ = 0;
        live_ask_px_ = kInvalidPrice;
    }
}

void MarketMaker::cancel_all() {
    if (live_bid_id_ != 0) {
        oms_.cancel(0, live_bid_id_);
        live_bid_id_ = 0;
        live_bid_px_ = kInvalidPrice;
    }
    if (live_ask_id_ != 0) {
        oms_.cancel(0, live_ask_id_);
        live_ask_id_ = 0;
        live_ask_px_ = kInvalidPrice;
    }
}

void MarketMaker::try_submit(Timestamp ts, Side side, Price px) {
    std::string reason;
    if (!risk_.check_new_order(cfg_.sym, side, cfg_.quote_size, reason)) {
        LMM_LOG(Debug, "mm") << "risk reject " << side_str(side)
                             << " reason=" << reason;
        return;
    }
    OrderId id = oms_.submit(ts, cfg_.sym, side, px, cfg_.quote_size);
    if (side == Side::Buy)  { live_bid_id_ = id; live_bid_px_ = px; }
    else                    { live_ask_id_ = id; live_ask_px_ = px; }
}

} // namespace lmm
