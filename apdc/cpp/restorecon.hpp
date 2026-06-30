#pragma once
// SELinux context restore via security.selinux xattr. Mirrors ../apd/src/restorecon.rs.
#include <string>

namespace apd::restorecon {

inline constexpr const char *SYSTEM_CON = "u:object_r:system_file:s0";
inline constexpr const char *ADB_CON = "u:object_r:adb_data_file:s0";
inline constexpr const char *UNLABEL_CON = "u:object_r:unlabeled:s0";

void lsetfilecon(const std::string &path, const std::string &con);
std::string lgetfilecon(const std::string &path);
void setsyscon(const std::string &path); // lsetfilecon(path, SYSTEM_CON)

// Recursively label dir (and itself) with SYSTEM_CON.
void restore_syscon(const std::string &dir);

// /data/adb/apd -> ADB_CON, then relabel unlabeled entries under modules to SYSTEM_CON.
void restorecon();

} // namespace apd::restorecon
