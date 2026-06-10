#pragma once

// Lightweight structured logger. Deliberately uses only <iostream> and
// friends rather than spdlog so the build has no external dependencies.
// Log level is a compile/runtime switch; log lines are tab-separated so
// they are easy to grep and easy to load into pandas.
//
// Usage:
//   LMM_LOG(INFO, "oms") << "sent order id=" << id;

#include <iostream>
#include <sstream>
#include <mutex>
#include <string>

namespace lmm {

enum class LogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void set_level(LogLevel l) { level_ = l; }
    LogLevel level() const { return level_; }

    void set_sim_time(std::int64_t ns) { sim_time_ns_ = ns; }
    std::int64_t sim_time() const { return sim_time_ns_; }

    void write(LogLevel l, const char* component, const std::string& msg) {
        if (l < level_) return;
        std::lock_guard<std::mutex> g(mu_);
        std::cerr << sim_time_ns_ << "\t" << level_str(l) << "\t"
                  << component << "\t" << msg << "\n";
    }

private:
    Logger() = default;
    static const char* level_str(LogLevel l) {
        switch (l) {
            case LogLevel::Trace: return "TRACE";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO ";
            case LogLevel::Warn:  return "WARN ";
            case LogLevel::Error: return "ERROR";
        }
        return "?";
    }
    LogLevel level_ = LogLevel::Info;
    std::int64_t sim_time_ns_ = 0;
    std::mutex mu_;
};

// Stream helper so the call site reads naturally.
class LogLine {
public:
    LogLine(LogLevel l, const char* c) : level_(l), component_(c) {}
    ~LogLine() { Logger::instance().write(level_, component_, oss_.str()); }
    template <typename T>
    LogLine& operator<<(const T& v) { oss_ << v; return *this; }
private:
    LogLevel level_;
    const char* component_;
    std::ostringstream oss_;
};

} // namespace lmm

#define LMM_LOG(LVL, COMP) ::lmm::LogLine(::lmm::LogLevel::LVL, COMP)
