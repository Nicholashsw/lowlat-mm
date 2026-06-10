#include "Events.hpp"

namespace lmm {

const char* to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::New:             return "New";
        case OrderStatus::PendingNew:      return "PendingNew";
        case OrderStatus::Working:         return "Working";
        case OrderStatus::PendingCancel:   return "PendingCancel";
        case OrderStatus::Cancelled:       return "Cancelled";
        case OrderStatus::PartiallyFilled: return "PartiallyFilled";
        case OrderStatus::Filled:          return "Filled";
        case OrderStatus::Rejected:        return "Rejected";
    }
    return "?";
}

} // namespace lmm
