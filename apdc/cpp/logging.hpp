#pragma once
// Logging shim. Mirrors Rust `log::{info,warn,error,debug}` routed through
// android_logger (logcat tag "APatchD"). User-facing output still uses printf.
#include <format>
#include <string_view>
#include <utility>

namespace apd::log {

// prio values are android/log.h ANDROID_LOG_* (DEBUG=3, INFO=4, WARN=5, ERROR=6).
void write_line(int prio, std::string_view msg);
void init();

template <class... A>
void debug(std::format_string<A...> f, A &&...a) {
    write_line(3, std::format(f, std::forward<A>(a)...));
}
template <class... A>
void info(std::format_string<A...> f, A &&...a) {
    write_line(4, std::format(f, std::forward<A>(a)...));
}
template <class... A>
void warn(std::format_string<A...> f, A &&...a) {
    write_line(5, std::format(f, std::forward<A>(a)...));
}
template <class... A>
void error(std::format_string<A...> f, A &&...a) {
    write_line(6, std::format(f, std::forward<A>(a)...));
}

} // namespace apd::log
