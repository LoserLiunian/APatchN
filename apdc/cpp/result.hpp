#pragma once
// anyhow-style error handling: subsystem functions throw apd::Error on failure;
// main() catches at the top, logs, and returns non-zero. Mirrors Rust's
// `anyhow::Result` + `?` + `.context()` + `bail!`/`ensure!`.
#include <format>
#include <stdexcept>
#include <string>
#include <utility>

namespace apd {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] inline void bail(const std::string &msg) { throw Error(msg); }

inline void ensure(bool cond, const std::string &msg) {
    if (!cond) throw Error(msg);
}

template <class... A>
[[noreturn]] void bailf(std::format_string<A...> f, A &&...a) {
    throw Error(std::format(f, std::forward<A>(a)...));
}

template <class... A>
void ensuref(bool cond, std::format_string<A...> f, A &&...a) {
    if (!cond) throw Error(std::format(f, std::forward<A>(a)...));
}

// Attach context to an in-flight error, like anyhow's `.with_context()`.
[[noreturn]] inline void rethrow_with_context(const std::exception &e,
                                              const std::string &ctx) {
    throw Error(ctx + ": " + e.what());
}

} // namespace apd
