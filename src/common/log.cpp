#include "common/log.h"

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

namespace phc::log {

namespace {
Level g_level = [] {
    if (const char* env = std::getenv("PHC_LOG")) {
        std::string s = env;
        if (s == "debug") return Level::Debug;
        if (s == "info")  return Level::Info;
        if (s == "warn")  return Level::Warn;
        if (s == "error") return Level::Error;
    }
    return Level::Info;
}();
std::mutex g_mu;

const char* tag(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?    ";
}
}  // namespace

void set_level(Level lvl) { g_level = lvl; }
Level current_level() { return g_level; }

namespace detail {
void emit(Level lvl, std::string_view msg) {
    std::lock_guard lk(g_mu);
    std::cerr << "[" << tag(lvl) << "] " << msg << "\n";
}
}  // namespace detail

}  // namespace phc::log
