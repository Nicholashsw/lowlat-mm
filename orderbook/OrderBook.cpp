#include "OrderBook.hpp"

namespace lmm {

void OrderBook::apply_top(Price bid_px, Qty bid_sz, Price ask_px, Qty ask_sz) {
    bids_.clear();
    asks_.clear();
    if (bid_px != kInvalidPrice && bid_sz > 0) bids_[bid_px] = bid_sz;
    if (ask_px != kInvalidPrice && ask_sz > 0) asks_[ask_px] = ask_sz;
}

void OrderBook::apply_level(Side side, Price px, Qty size) {
    auto& book = (side == Side::Buy) ? bids_ : asks_;
    if (size <= 0) {
        book.erase(px);
    } else {
        book[px] = size;
    }
}

Price OrderBook::mid() const {
    if (!two_sided() || crossed()) return kInvalidPrice;
    return (best_bid() + best_ask()) / 2;
}

Price OrderBook::spread() const {
    if (!two_sided()) return kInvalidPrice;
    return best_ask() - best_bid();
}

} // namespace lmm
