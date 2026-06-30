#pragma once
// Bundled binary paths + setup. Mirrors ../apd/src/assets.rs.
namespace apd::assets {

inline constexpr const char *RESETPROP_PATH = "/data/adb/ap/bin/resetprop";
inline constexpr const char *BUSYBOX_PATH = "/data/adb/ap/bin/busybox";
inline constexpr const char *MAGISKPOLICY_PATH = "/data/adb/ap/bin/magiskpolicy";

// chmod busybox + (re)create resetprop/magiskpolicy symlinks to /data/adb/apd.
void ensure_binaries();

} // namespace apd::assets
