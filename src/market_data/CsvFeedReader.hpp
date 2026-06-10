#pragma once

// CSV top-of-book feed reader. File format (header required):
//
//   ts_ns,symbol,bid_px,bid_sz,ask_px,ask_sz
//   0,TEST,10000,100,10002,100
//   ...
//
// Prices in the CSV are already in tick units (integers). The caller supplies
// the tick size for logging / conversion purposes; the engine itself stays
// in tick units end to end.
//
// This is deliberately simple. A real feed handler would demux multiple
// symbols, handle gaps, and emit L2 updates. Here we just stream a vector of
// TopOfBookEvents in timestamp order.

#include "../core/Events.hpp"
#include <string>
#include <vector>
#include <unordered_map>

namespace lmm {

struct SymbolTable {
    // symbol string -> id
    std::unordered_map<std::string, SymbolId> s2i;
    std::vector<std::string> i2s;

    SymbolId intern(const std::string& s) {
        auto it = s2i.find(s);
        if (it != s2i.end()) return it->second;
        SymbolId id = static_cast<SymbolId>(i2s.size());
        s2i.emplace(s, id);
        i2s.push_back(s);
        return id;
    }
    const std::string& name(SymbolId id) const { return i2s.at(id); }
};

class CsvFeedReader {
public:
    // Reads the whole file into memory. Keep files small for the prototype.
    // Returns events sorted by (ts, line-order). Throws std::runtime_error
    // on malformed input with line number.
    static std::vector<TopOfBookEvent> read(const std::string& path,
                                            SymbolTable& symbols);
};

} // namespace lmm
