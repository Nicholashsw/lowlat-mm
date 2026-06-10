#pragma once

// Tiny key=value config reader. Deliberately minimal: no INI sections, no
// JSON dependency. Comment lines start with '#'. Values are parsed on demand
// by the caller via get<T>. Keeps the prototype self-contained.

#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <optional>

namespace lmm {

class Config {
public:
    static Config load(const std::string& path) {
        Config c;
        std::ifstream in(path);
        if (!in) throw std::runtime_error("Config: cannot open " + path);
        std::string line;
        while (std::getline(in, line)) {
            auto hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(line.substr(0, eq));
            std::string v = trim(line.substr(eq + 1));
            if (!k.empty()) c.kv_[k] = v;
        }
        return c;
    }

    bool has(const std::string& k) const { return kv_.count(k) > 0; }

    std::string get_str(const std::string& k, const std::string& def = "") const {
        auto it = kv_.find(k);
        return it == kv_.end() ? def : it->second;
    }

    double get_double(const std::string& k, double def) const {
        auto it = kv_.find(k);
        if (it == kv_.end()) return def;
        return std::stod(it->second);
    }

    long long get_int(const std::string& k, long long def) const {
        auto it = kv_.find(k);
        if (it == kv_.end()) return def;
        return std::stoll(it->second);
    }

    bool get_bool(const std::string& k, bool def) const {
        auto it = kv_.find(k);
        if (it == kv_.end()) return def;
        const std::string& v = it->second;
        return v == "1" || v == "true" || v == "True" || v == "yes";
    }

private:
    static std::string trim(std::string s) {
        auto notspace = [](int ch){ return !std::isspace(ch); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
        return s;
    }
    std::unordered_map<std::string, std::string> kv_;
};

} // namespace lmm
