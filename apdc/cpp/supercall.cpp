#include "supercall.hpp"
#include "logging.hpp"
#include "package.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace apd::supercall {
namespace {

// apd-specific supercall version (see header note).
constexpr long MAJOR = 0;
constexpr long MINOR = 11;
constexpr long PATCH = 1;

constexpr long NR_SUPERCALL = 45; // overlays __NR_truncate

constexpr long SC_SU = 0x1010;
constexpr long SC_KSTORAGE_WRITE = 0x1041;
constexpr long SC_SU_GRANT_UID = 0x1100;
constexpr long SC_SU_REVOKE_UID = 0x1101;
constexpr long SC_SU_NUMS = 0x1102;
constexpr long SC_SU_LIST = 0x1103;
constexpr long SC_SU_RESET_PATH = 0x1111;
constexpr long SC_SU_GET_SAFEMODE = 0x1112;

constexpr int KSTORAGE_EXCLUDE_LIST_GROUP = 1;
constexpr std::size_t SCONTEXT_LEN = 0x60;

struct SuProfile {
    int32_t uid;
    int32_t to_uid;
    uint8_t scontext[SCONTEXT_LEN];
};

long ver_and_cmd(long cmd) {
    uint32_t version_code = static_cast<uint32_t>((MAJOR << 16) + (MINOR << 8) + PATCH);
    return (static_cast<long>(version_code) << 32) | (0x1158L << 16) | (cmd & 0xFFFF);
}

template <class... Args>
long sc(const char *key, long cmd, Args... args) {
    return static_cast<long>(syscall(NR_SUPERCALL, key, ver_and_cmd(cmd), args...));
}

bool empty_key(const std::string &key) { return key.empty(); }

[[maybe_unused]] long sc_su_revoke_uid(const std::string &key, uid_t uid) {
    if (empty_key(key)) return -EINVAL;
    return sc(key.c_str(), SC_SU_REVOKE_UID, uid);
}

[[maybe_unused]] long sc_su_grant_uid(const std::string &key, const SuProfile *profile) {
    if (empty_key(key)) return -EINVAL;
    return sc(key.c_str(), SC_SU_GRANT_UID, profile);
}

[[maybe_unused]] long sc_kstorage_write(const std::string &key, int gid, int64_t did,
                                        void *data, int offset, int dlen) {
    if (empty_key(key)) return -EINVAL;
    long packed = (static_cast<int64_t>(offset) << 32) | static_cast<int64_t>(dlen);
    return sc(key.c_str(), SC_KSTORAGE_WRITE, static_cast<long>(gid),
              static_cast<long>(did), data, packed);
}

[[maybe_unused]] long sc_set_ap_mod_exclude(const std::string &key, int64_t uid, int exclude) {
    return sc_kstorage_write(key, KSTORAGE_EXCLUDE_LIST_GROUP, uid, &exclude, 0,
                             static_cast<int>(sizeof(int)));
}

long sc_su(const std::string &key, const SuProfile *profile) {
    if (empty_key(key)) return -EINVAL;
    return sc(key.c_str(), SC_SU, profile);
}

long sc_su_reset_path(const std::string &key, const std::string &path) {
    if (empty_key(key) || path.empty()) return -EINVAL;
    return sc(key.c_str(), SC_SU_RESET_PATH, path.c_str());
}

[[maybe_unused]] long sc_su_uid_nums(const std::string &key) {
    if (empty_key(key)) return -EINVAL;
    return sc(key.c_str(), SC_SU_NUMS);
}

[[maybe_unused]] long sc_su_allow_uids(const std::string &key, uid_t *buf, int len) {
    if (empty_key(key) || len <= 0) return -EINVAL;
    return sc(key.c_str(), SC_SU_LIST, buf, len);
}

void copy_scontext(uint8_t (&dst)[SCONTEXT_LEN], const std::string &s) {
    std::memset(dst, 0, SCONTEXT_LEN);
    std::size_t len = std::min(SCONTEXT_LEN, s.size());
    std::memcpy(dst, s.data(), len);
}

} // namespace

long su_get_safemode(const std::string &key) {
    if (empty_key(key)) {
        log::warn("[sc_su_get_safemode] null superkey, tell apd we are not in safemode!");
        return 0;
    }
    return sc(key.c_str(), SC_SU_GET_SAFEMODE);
}

void privilege_apd_profile(const OptStr &superkey) {
    if (!superkey) return;

    SuProfile profile{};
    profile.uid = static_cast<int32_t>(getpid());
    profile.to_uid = 0;
    copy_scontext(profile.scontext, "u:r:magisk:s0");

    long result = sc_su(*superkey, &profile);
    log::info("[privilege_apd_profile] result = {}", result);
}

void init_load_su_path(const OptStr &superkey) {
    const char *su_path_file = "/data/adb/ap/su_path";

    std::ifstream in(su_path_file);
    if (!in) {
        log::warn("Failed to read su_path file");
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string su_path = ss.str();
    // trim() — match Rust str::trim ASCII set.
    auto b = su_path.find_first_not_of(" \t\n\v\f\r");
    auto e = su_path.find_last_not_of(" \t\n\v\f\r");
    su_path = (b == std::string::npos) ? "" : su_path.substr(b, e - b + 1);

    // A NUL inside the trimmed path would silently truncate when handed to the
    // kernel; Rust treats this as invalid and skips. Mirror that.
    if (su_path.find('\0') != std::string::npos) {
        log::warn("su_path file contains NUL byte; refusing to load");
        return;
    }

    if (!superkey) {
        log::warn("Superkey is None, skipping...");
        return;
    }
    long result = sc_su_reset_path(*superkey, su_path);
    if (result == 0) {
        log::info("suPath load successfully");
    } else {
        log::warn("Failed to load su path, error code: {}", result);
    }
}

void refresh_ap_package_list(const std::string &skey, std::mutex &mtx) {
    std::lock_guard<std::mutex> lock(mtx);

    long num = sc_su_uid_nums(skey);
    if (num < 0) {
        log::error("[refresh_su_list] Error getting number of UIDs: {}", num);
        return;
    }
    std::vector<uid_t> uids(static_cast<std::size_t>(num), 0);
    long n = sc_su_allow_uids(skey, uids.data(), static_cast<int>(uids.size()));
    if (n < 0) {
        log::error("[refresh_su_list] Error getting su list");
        return;
    }
    for (uid_t uid : uids) {
        if (uid == 0 || uid == 2000) {
            log::warn("[refresh_ap_package_list] Skip revoking critical uid: {}", uid);
            continue;
        }
        log::info("[refresh_ap_package_list] Revoking {} root permission...", uid);
        long rc = sc_su_revoke_uid(skey, uid);
        if (rc != 0) log::error("[refresh_ap_package_list] Error revoking UID: {}", rc);
    }

    try {
        package::synchronize_package_uid();
    } catch (const std::exception &e) {
        log::error("Failed to synchronize package UIDs: {}", e.what());
    }

    for (const auto &c : package::read_ap_package_config()) {
        if (c.allow == 1 && c.exclude == 0) {
            SuProfile profile{};
            profile.uid = c.uid;
            profile.to_uid = c.to_uid;
            copy_scontext(profile.scontext, c.sctx);
            long result = sc_su_grant_uid(skey, &profile);
            log::info("[refresh_ap_package_list] Loading {}: result = {}", c.pkg, result);
        }
        if (c.allow == 0 && c.exclude == 1) {
            long result = sc_set_ap_mod_exclude(skey, static_cast<int64_t>(c.uid), 1);
            log::info("[refresh_ap_package_list] Loading exclude {}: result = {}", c.pkg, result);
        }
    }
}

} // namespace apd::supercall
