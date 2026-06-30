#pragma once
// Path constants and shared aliases. Mirrors ../apd/src/defs.rs.
#include <optional>
#include <string>
#include "version.hpp"

namespace apd {
using OptStr = std::optional<std::string>;
}

namespace apd::defs {
inline constexpr const char *ADB_DIR = "/data/adb/";
inline constexpr const char *WORKING_DIR = "/data/adb/ap/";
inline constexpr const char *BINARY_DIR = "/data/adb/ap/bin/";
inline constexpr const char *APATCH_LOG_FOLDER = "/data/adb/ap/log/";

inline constexpr const char *AP_RC_PATH = "/data/adb/ap/.aprc";
inline constexpr const char *GLOBAL_NAMESPACE_FILE = "/data/adb/.global_namespace_enable";
inline constexpr const char *DAEMON_PATH = "/data/adb/apd";

inline constexpr const char *MODULE_DIR = "/data/adb/modules/";
// warning: must match module_installer.sh!
inline constexpr const char *MODULE_UPDATE_DIR = "/data/adb/modules_update/";

inline constexpr const char *TEMP_DIR = "/debug_ramdisk";
inline constexpr const char *TEMP_DIR_LEGACY = "/sbin";

inline constexpr const char *MODULE_WEB_DIR = "webroot";
inline constexpr const char *MODULE_ACTION_SH = "action.sh";
inline constexpr const char *DISABLE_FILE_NAME = "disable";
inline constexpr const char *UPDATE_FILE_NAME = "update";
inline constexpr const char *REMOVE_FILE_NAME = "remove";

// Metamodule support
inline constexpr const char *METAMODULE_MOUNT_SCRIPT = "metamount.sh";
inline constexpr const char *METAMODULE_METAINSTALL_SCRIPT = "metainstall.sh";
inline constexpr const char *METAMODULE_METAUNINSTALL_SCRIPT = "metauninstall.sh";
inline constexpr const char *METAMODULE_DIR = "/data/adb/metamodule/";

// Module config
inline constexpr const char *MODULE_CONFIG_DIR = "/data/adb/ap/module_configs/";
inline constexpr const char *PERSIST_CONFIG_NAME = "persist.config";
inline constexpr const char *TEMP_CONFIG_NAME = "tmp.config";

inline constexpr const char *PTS_NAME = "pts";

inline constexpr const char *VERSION_CODE = APD_VERSION_CODE;
inline constexpr const char *VERSION_NAME = APD_VERSION_NAME;
} // namespace apd::defs
