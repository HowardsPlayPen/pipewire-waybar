#pragma once

#include <iostream>
#include <sstream>
#include <string_view>

namespace phc::log {

enum class Level { Debug, Info, Warn, Error };

void set_level(Level lvl);
Level current_level();

namespace detail {
void emit(Level lvl, std::string_view msg);
}

template <class... Args>
void at(Level lvl, Args&&... args) {
    if (lvl < current_level()) return;
    std::ostringstream oss;
    (oss << ... << std::forward<Args>(args));
    detail::emit(lvl, oss.str());
}

template <class... Args> void debug(Args&&... a) { at(Level::Debug, std::forward<Args>(a)...); }
template <class... Args> void info (Args&&... a) { at(Level::Info,  std::forward<Args>(a)...); }
template <class... Args> void warn (Args&&... a) { at(Level::Warn,  std::forward<Args>(a)...); }
template <class... Args> void error(Args&&... a) { at(Level::Error, std::forward<Args>(a)...); }

}  // namespace phc::log
