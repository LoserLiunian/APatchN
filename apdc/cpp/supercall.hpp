#pragma once
// KernelPatch supercall interface. Mirrors ../apd/src/supercall.rs.
// NOTE: apd's supercall version is independently 0/11/1 (NOT the KernelPatch
// `version` file). Command numbers/structs match app/.../uapi/scdefs.h.
#include <mutex>
#include <string>
#include "defs.hpp"

namespace apd::supercall {

// SUPERCALL_SU_GET_SAFEMODE. Empty key -> treated as "not safemode" (returns 0).
long su_get_safemode(const std::string &key);

// Re-privilege the apd process to u:r:magisk:s0 via SUPERCALL_SU.
void privilege_apd_profile(const OptStr &superkey);

// Load /data/adb/ap/su_path into the kernel via SUPERCALL_SU_RESET_PATH.
void init_load_su_path(const OptStr &superkey);

// Revoke all current su uids, sync package list, then re-grant/exclude from
// package_config. Serialized by `mtx` (shared with the uid-listener thread).
void refresh_ap_package_list(const std::string &skey, std::mutex &mtx);

} // namespace apd::supercall
