#include "utils.hpp"
#include "logging.hpp"
#include "result.hpp"
#include "supercall.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace apd::utils {

void ensure_file_exists(const std::string &path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd >= 0) {
        ::close(fd);
        return;
    }
    if (errno == EEXIST) {
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) return;
        bailf("{} is not a regular file", path);
    }
    bailf("failed to create {}: {}", path, std::strerror(errno));
}

void ensure_dir_exists(const std::string &path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (fs::is_directory(path, ec)) return;
    bailf("{} is not a regular directory", path);
}

void ensure_binary(const std::string &path) {
    if (::chmod(path.c_str(), 0755) != 0)
        bailf("failed to chmod {}: {}", path, std::strerror(errno));
}

OptStr getprop(const std::string &prop) {
    char value[PROP_VALUE_MAX] = {0};
    int len = __system_property_get(prop.c_str(), value);
    if (len <= 0) return std::nullopt;
    return std::string(value, static_cast<std::size_t>(len));
}

bool is_safe_mode(const OptStr &superkey) {
    bool safemode = (getprop("persist.sys.safemode") == OptStr("1")) ||
                    (getprop("ro.sys.safemode") == OptStr("1"));
    log::info("safemode: {}", safemode);
    if (safemode) return true;

    if (!superkey) {
        log::warn("[is_safe_mode] No valid superkey provided, assuming safemode as false.");
        return false;
    }
    bool kernel_safemode = supercall::su_get_safemode(*superkey) == 1;
    log::info("kernel_safemode: {}", kernel_safemode);
    return kernel_safemode;
}

void switch_mnt_ns(int pid) {
    std::string path = std::format("/proc/{}/ns/mnt", pid);
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) bailf("open {} failed: {}", path, std::strerror(errno));

    std::error_code ec;
    fs::path cwd = fs::current_path(ec);

    int ret = ::setns(fd, CLONE_NEWNS);
    ::close(fd);

    if (!ec) {
        std::error_code ec2;
        fs::current_path(cwd, ec2);
    }
    ensure(ret == 0, "switch mnt ns failed");
}

static void switch_cgroup(const char *grp, pid_t pid) {
    std::string path = std::string(grp) + "/cgroup.procs";
    std::error_code ec;
    if (!fs::exists(path, ec)) return;

    int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
    if (fd < 0) return;
    std::string s = std::to_string(pid);
    (void)!::write(fd, s.data(), s.size());
    ::close(fd);
}

void switch_cgroups() {
    pid_t pid = ::getpid();
    switch_cgroup("/acct", pid);
    switch_cgroup("/dev/cg2_bpf", pid);
    switch_cgroup("/sys/fs/cgroup", pid);

    if (getprop("ro.config.per_app_memcg") != OptStr("false")) {
        switch_cgroup("/dev/memcg/apps", pid);
    }
}

void umask(unsigned mask) { ::umask(static_cast<mode_t>(mask)); }

bool has_magisk() {
    const char *penv = ::getenv("PATH");
    if (!penv) return false;
    std::stringstream ss(penv);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        // Match Rust `which::which` semantics: an empty PATH segment is treated
        // as the current working directory; a candidate must be a regular file
        // (not a directory named "magisk") AND executable.
        std::string base = dir.empty() ? std::string(".") : dir;
        std::string candidate = base + "/magisk";
        struct stat st{};
        if (::stat(candidate.c_str(), &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        if (::access(candidate.c_str(), X_OK) == 0) return true;
    }
    return false;
}

std::string get_tmp_path() {
    struct stat st{};
    if (::stat(defs::TEMP_DIR_LEGACY, &st) == 0) return defs::TEMP_DIR_LEGACY;
    if (::stat(defs::TEMP_DIR, &st) == 0) return defs::TEMP_DIR;
    return "";
}

} // namespace apd::utils
