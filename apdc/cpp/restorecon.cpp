#include "restorecon.hpp"
#include "defs.hpp"
#include "result.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sys/xattr.h>
#include <vector>

namespace fs = std::filesystem;

namespace apd::restorecon {
namespace {

constexpr const char *SELINUX_XATTR = "security.selinux";

// Walk dir AND the dir itself (mirrors jwalk WalkDir, which yields the root).
template <class F> void walk(const std::string &dir, F &&f) {
    f(fs::path(dir));
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator();) {
        if (ec) {
            // Rust's jwalk skips the problematic entry and continues — do the
            // same so a single transient EIO/ENOENT mid-walk does not leave
            // every subsequent entry unlabeled.
            ec.clear();
            it.increment(ec);
            continue;
        }
        f(it->path());
        it.increment(ec);
    }
}

} // namespace

void lsetfilecon(const std::string &path, const std::string &con) {
    if (::lsetxattr(path.c_str(), SELINUX_XATTR, con.data(), con.size(), 0) != 0)
        bailf("Failed to change SELinux context for {}: {}", path, std::strerror(errno));
}

std::string lgetfilecon(const std::string &path) {
    ssize_t len = ::lgetxattr(path.c_str(), SELINUX_XATTR, nullptr, 0);
    if (len < 0) bailf("Failed to get SELinux context for {}: {}", path, std::strerror(errno));
    std::vector<char> buf(static_cast<std::size_t>(len));
    len = ::lgetxattr(path.c_str(), SELINUX_XATTR, buf.data(), buf.size());
    if (len < 0) bailf("Failed to get SELinux context for {}: {}", path, std::strerror(errno));
    return std::string(buf.data(), static_cast<std::size_t>(len));
}

void setsyscon(const std::string &path) { lsetfilecon(path, SYSTEM_CON); }

void restore_syscon(const std::string &dir) {
    walk(dir, [](const fs::path &p) { setsyscon(p.string()); });
}

static void restore_syscon_if_unlabeled(const std::string &dir) {
    walk(dir, [](const fs::path &p) {
        std::string con;
        try {
            con = lgetfilecon(p.string());
        } catch (const std::exception &) {
            return; // matches Rust: only relabel when get succeeds
        }
        if (con == UNLABEL_CON || con.empty()) lsetfilecon(p.string(), SYSTEM_CON);
    });
}

void restorecon() {
    lsetfilecon(defs::DAEMON_PATH, ADB_CON);
    restore_syscon_if_unlabeled(defs::MODULE_DIR);
}

} // namespace apd::restorecon
