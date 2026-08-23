#pragma once

// Order book abstraction.
//
// Design notes:
//  - For the prototype we only track top-of-book (best bid/ask + size) plus
//    a small number of extra levels stored as std::map for auto-sorting.
//    std::map is obviously not the data structure you would use in a real
//    HFT book (you'd use a pre-allocated vector of price buckets keyed by
//    tick index), but it keeps the code clear and fast enough at the volumes
//    a backtest generates.
//  - The book is updated either by TopOfBookEvent (snapshot replace) or by
//    explicit level updates via apply_level(). The simulator only needs the
//    former.
//  - No internal matching here; matching logic lives in the simulator. The
//    book is just a view of current market state.

#include "../core/Types.hpp"
#include "../core/Events.hpp"
#include <map>
#include <optional>

namespace lmm {

class OrderBook {
public:
    explicit OrderBook(SymbolId sym = 0) : sym_(sym) {}

    SymbolId symbol() const { return sym_; }

    // Replace the top of book atomically. All deeper levels are cleared
    // because a ToB snapshot does not carry depth information and mixing
    // stale depth with new top would be misleading.
    void apply_top(Price bid_px, Qty bid_sz, Price ask_px, Qty ask_sz);

    // Apply an explicit per-level update. size == 0 removes the level.
    void apply_level(Side side, Price px, Qty size);

    // Accessors -----------------------------------------------------------
    bool has_bid() const { return !bids_.empty(); }
    bool has_ask() const { return !asks_.empty(); }
    bool two_sided() const { return has_bid() && has_ask(); }

    Price best_bid() const { return has_bid() ? bids_.rbegin()->first : kInvalidPrice; }
    Price best_ask() const { return has_ask() ? asks_.begin()->first  : kInvalidPrice; }
    Qty   best_bid_size() const { return has_bid() ? bids_.rbegin()->second : 0; }
    Qty   best_ask_size() const { return has_ask() ? asks_.begin()->second  : 0; }

    // Mid in ticks (rounded down when the spread is odd). Returns
    // kInvalidPrice when the book is one-sided or crossed.
    Price mid() const;

    // Spread in ticks; kInvalidPrice if undefined.
    Price spread() const;

    // True when the book is malformed (bid >= ask). Strategies should
    // refuse to quote on a crossed book.
    bool crossed() const {
        return two_sided() && best_bid() >= best_ask();
    }

private:
    SymbolId sym_;
    // Bids sorted ascending, so best bid is rbegin(). Asks sorted ascending,
    // best ask is begin(). Keeping both ascending avoids confusion.
    std::map<Price, Qty> bids_;
    std::map<Price, Qty> asks_;
};

} // namespace lmm
