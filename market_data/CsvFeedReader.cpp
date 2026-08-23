#include "CsvFeedReader.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace lmm {

namespace {

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

} // namespace

std::vector<TopOfBookEvent> CsvFeedReader::read(const std::string& path,
                                                SymbolTable& symbols) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("CsvFeedReader: cannot open " + path);

    std::vector<TopOfBookEvent> events;
    std::string line;
    std::size_t lineno = 0;

    // Header
    if (!std::getline(in, line))
        throw std::runtime_error("CsvFeedReader: empty file " + path);
    ++lineno;

    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty()) continue;
        auto cols = split(line, ',');
        if (cols.size() < 6) {
            throw std::runtime_error("CsvFeedReader: too few cols at " +
                                     path + ":" + std::to_string(lineno));
        }
        try {
            TopOfBookEvent e{};
            e.ts      = std::stoll(cols[0]);
            e.sym     = symbols.intern(cols[1]);
            e.bid_px  = std::stoll(cols[2]);
            e.bid_sz  = std::stoll(cols[3]);
            e.ask_px  = std::stoll(cols[4]);
            e.ask_sz  = std::stoll(cols[5]);
            events.push_back(e);
        } catch (const std::exception& ex) {
            throw std::runtime_error("CsvFeedReader: parse error at " +
                                     path + ":" + std::to_string(lineno) +
                                     " - " + ex.what());
        }
    }

    // Stable sort by timestamp so ties preserve file order (matters for
    // deterministic replay).
    std::stable_sort(events.begin(), events.end(),
                     [](const TopOfBookEvent& a, const TopOfBookEvent& b) {
                         return a.ts < b.ts;
                     });
    return events;
}

} // namespace lmm
