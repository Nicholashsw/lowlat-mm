#include "ExchangeSimulator.hpp"
#include "../core/Logger.hpp"

namespace lmm {

void ExchangeSimulator::on_new_order(const NewOrderEvent& e) {
    // Always schedule an ack (accepted). The risk gate has already approved.
    AckEvent ack{e.id, e.ts + cfg_.ack_latency_ns, true, ""};
    emit_(ack.ts, Event{ack});

    Resting r{e, e.qty};

    // Marketable check against current book snapshot.
    auto it = books_.find(e.sym);
    if (cfg_.allow_marketable && it != books_.end()) {
        const auto& book = it->second;
        if (e.side == Side::Buy && book.has_ask() && e.px >= book.best_ask()) {
            emit_fill(r, book.best_ask(), r.leaves,
                      e.ts + cfg_.fill_latency_ns, true);
            return;
        }
        if (e.side == Side::Sell && book.has_bid() && e.px <= book.best_bid()) {
            emit_fill(r, book.best_bid(), r.leaves,
                      e.ts + cfg_.fill_latency_ns, true);
            return;
        }
    }

    resting_[e.id] = r;
}

void ExchangeSimulator::on_cancel_order(const CancelOrderEvent& e) {
    auto it = resting_.find(e.id);
    bool accepted = (it != resting_.end());
    if (accepted) resting_.erase(it);
    CancelAckEvent ack{e.id, e.ts + cfg_.ack_latency_ns, accepted};
    emit_(ack.ts, Event{ack});
}

void ExchangeSimulator::on_market_data(const TopOfBookEvent& md) {
    auto& book = books_[md.sym];
    book.apply_top(md.bid_px, md.bid_sz, md.ask_px, md.ask_sz);

    // Walk resting orders and fill any that are through the market. We
    // must iterate over a copy of the keys because emit_fill may erase from
    // resting_.
    std::vector<OrderId> ids;
    ids.reserve(resting_.size());
    for (auto& [id, r] : resting_) {
        if (r.ev.sym == md.sym) ids.push_back(id);
    }
    for (OrderId id : ids) {
        auto it = resting_.find(id);
        if (it == resting_.end()) continue;
        auto& r = it->second;
        maybe_fill_against_book(r, md.ts);
    }
}

void ExchangeSimulator::maybe_fill_against_book(Resting& r, Timestamp ts) {
    auto& book = books_[r.ev.sym];
    // Passive fill rule: resting order fills only if the market trades
    // THROUGH it (strict inequality), not AT the touch. This avoids
    // over-optimistic fills for a market maker.
    if (r.ev.side == Side::Buy && book.has_ask() && book.best_ask() < r.ev.px) {
        // Market traded through our bid.
        Qty fill = r.leaves;
        emit_fill(r, r.ev.px, fill, ts + cfg_.fill_latency_ns, true);
        resting_.erase(r.ev.id);
        return;
    }
    if (r.ev.side == Side::Sell && book.has_bid() && book.best_bid() > r.ev.px) {
        Qty fill = r.leaves;
        emit_fill(r, r.ev.px, fill, ts + cfg_.fill_latency_ns, true);
        resting_.erase(r.ev.id);
        return;
    }
}

void ExchangeSimulator::emit_fill(const Resting& r, Price px, Qty qty,
                                  Timestamp ts, bool is_final) {
    FillEvent f{r.ev.id, ts, px, qty, is_final};
    emit_(ts, Event{f});
    LMM_LOG(Trace, "sim") << "fill id=" << r.ev.id << " px=" << px
                          << " qty=" << qty;
}

} // namespace lmm
