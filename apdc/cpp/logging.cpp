#include "logging.hpp"
#include <android/log.h>
#include <string>

namespace apd::log {

void write_line(int prio, std::string_view msg) {
    // android/log.h wants a NUL-terminated string.
    std::string buf(msg);
    __android_log_write(prio, "APatchD", buf.c_str());
}

void init() {
    // android_logger needs no explicit init; kept for parity with cli.rs.
}

} // namespace apd::log
