#pragma once

// Event bus used by the backtest driver.
//
// This is a min-heap keyed on (timestamp, sequence). Sequence is an
// auto-incrementing tiebreaker so that events scheduled at exactly the same
// timestamp are processed in insertion order — this matters for replay
// determinism.
//
// In a real trading system you'd instead have multiple queues (one per
// thread / per feed) and lock-free handoff. For a single-threaded backtest,
// a sorted priority queue is simpler and equally correct.

#include "Events.hpp"
#include <queue>
#include <vector>
#include <functional>

namespace lmm {

class EventBus {
public:
    void push(Timestamp ts, Event e) {
        pq_.push({ts, seq_++, std::move(e)});
    }

    bool empty() const { return pq_.empty(); }
    std::size_t size() const { return pq_.size(); }

    // Caller owns the popped event. Undefined if empty.
    std::pair<Timestamp, Event> pop() {
        auto item = pq_.top();
        pq_.pop();
        return { item.ts, std::move(const_cast<Event&>(item.e)) };
    }

    Timestamp peek_ts() const { return pq_.top().ts; }

private:
    struct Item {
        Timestamp ts;
        std::uint64_t seq;
        Event e;
        // std::priority_queue is a max-heap; invert to get min-heap.
        bool operator<(const Item& other) const {
            if (ts != other.ts) return ts > other.ts;
            return seq > other.seq;
        }
    };
    std::priority_queue<Item> pq_;
    std::uint64_t seq_ = 0;
};

} // namespace lmm
