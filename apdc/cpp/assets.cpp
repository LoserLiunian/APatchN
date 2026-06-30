#include "assets.hpp"
#include "result.hpp"
#include "utils.hpp"

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace apd::assets {

void ensure_binaries() {
    utils::ensure_binary(BUSYBOX_PATH);

    ::unlink(RESETPROP_PATH);
    if (::symlink("/data/adb/apd", RESETPROP_PATH) != 0)
        bailf("Failed to symlink resetprop: {}", std::strerror(errno));

    ::unlink(MAGISKPOLICY_PATH);
    if (::symlink("/data/adb/apd", MAGISKPOLICY_PATH) != 0)
        bailf("Failed to symlink magiskpolicy: {}", std::strerror(errno));
}

} // namespace apd::assets
