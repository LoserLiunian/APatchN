#pragma once
// Unified shim for the Magisk `base` facilities used by the vendored v23
// magiskpolicy + resetprop sources. Resolves their `#include <utils.hpp>`.
// apd-specific: logging routes to logcat tag "APatchD".
#include <android/log.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Vendored Magisk code uses these unqualified.
using std::string;
using std::string_view;
using std::vector;

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "APatchD", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "APatchD", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "APatchD", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "APatchD", __VA_ARGS__)

// Magisk logging-callback holder. Our LOG* macros log directly, so the callback
// is never invoked; resetprop_main assigns log_cb.d (verbose) and magisk_rules
// saves/restores log_cb.w — both are no-ops here. Type matches Magisk exactly.
inline int nop_log(const char *, va_list) { return 0; }
struct log_callback {
    int (*d)(const char *, va_list) = nullptr;
    int (*i)(const char *, va_list) = nullptr;
    int (*w)(const char *, va_list) = nullptr;
    int (*e)(const char *, va_list) = nullptr;
};
inline log_callback log_cb;

static inline int xopen(const char *path, int flags) { return ::open(path, flags); }
static inline int xopen(const char *path, int flags, mode_t mode) {
    return ::open(path, flags, mode);
}
static inline FILE *xfopen(const char *path, const char *mode) { return ::fopen(path, mode); }

static inline void *xmalloc(size_t sz) { return ::malloc(sz); }
static inline void *xcalloc(size_t n, size_t sz) { return ::calloc(n, sz); }
static inline void *xrealloc(void *p, size_t sz) { return ::realloc(p, sz); }

static inline ssize_t xread(int fd, void *buf, size_t count) {
    auto *p = static_cast<char *>(buf);
    size_t total = 0;
    while (total < count) {
        ssize_t n = ::read(fd, p + total, count - total);
        if (n < 0) return -1;
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

static inline ssize_t xwrite(int fd, const void *buf, size_t count) {
    auto *p = static_cast<const char *>(buf);
    size_t total = 0;
    while (total < count) {
        ssize_t n = ::write(fd, p + total, count - total);
        if (n < 0) return -1;
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(total);
}

static inline std::unique_ptr<FILE, int (*)(FILE *)> xopen_file(const char *path,
                                                               const char *mode) {
    return {::fopen(path, mode), ::fclose};
}

static inline bool str_starts(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Map a file read-only; sets addr/size (addr=nullptr on failure).
template <class T> static inline void mmap_ro(const char *file, T *&addr, size_t &size) {
    addr = nullptr;
    size = 0;
    int fd = ::open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st{};
    if (::fstat(fd, &st) == 0 && st.st_size > 0) {
        size = static_cast<size_t>(st.st_size);
        void *p = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        addr = (p == MAP_FAILED) ? nullptr : static_cast<T *>(p);
        if (!addr) size = 0;
    }
    ::close(fd);
}

static inline std::unique_ptr<DIR, int (*)(DIR *)> open_dir(const char *path) {
    return {::opendir(path), ::closedir};
}

static inline dirent *xreaddir(DIR *dir) {
    for (dirent *e; (e = ::readdir(dir));) {
        if (::strcmp(e->d_name, ".") != 0 && ::strcmp(e->d_name, "..") != 0) return e;
    }
    return nullptr;
}

// Copy mode/owner/SELinux context from src to dst (best-effort).
static inline void clone_attr(const char *src, const char *dst) {
    struct stat st{};
    if (::stat(src, &st) != 0) return;
    ::chmod(dst, st.st_mode & 0777);
    ::chown(dst, st.st_uid, st.st_gid);
    char con[256];
    ssize_t len = ::lgetxattr(src, "security.selinux", con, sizeof(con));
    if (len > 0) ::lsetxattr(dst, "security.selinux", con, static_cast<size_t>(len), 0);
}

// Iterate lines; stop early when fn returns false. trim strips surrounding ws.
static inline void file_readline(bool trim, FILE *fp,
                                 const std::function<bool(std::string_view)> &fn) {
    char *line = nullptr;
    size_t cap = 0;
    ssize_t len;
    while ((len = ::getline(&line, &cap, fp)) >= 0) {
        size_t n = static_cast<size_t>(len);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        std::string_view sv(line, n);
        if (trim) {
            size_t b = sv.find_first_not_of(" \t");
            size_t e = sv.find_last_not_of(" \t");
            sv = (b == std::string_view::npos) ? std::string_view() : sv.substr(b, e - b + 1);
        }
        if (!fn(sv)) break;
    }
    ::free(line);
}

static inline void file_readline(bool trim, const char *file,
                                 const std::function<bool(std::string_view)> &fn) {
    FILE *fp = ::fopen(file, "re");
    if (fp == nullptr) return;
    file_readline(trim, fp, fn);
    ::fclose(fp);
}

// Parse a .prop file: skip blanks/#-comments, split on first '=', call fn(key,val).
static inline void parse_prop_file(const char *file,
                                   const std::function<bool(std::string_view, std::string_view)> &fn) {
    file_readline(true, file, [&](std::string_view line) -> bool {
        if (line.empty() || line[0] == '#') return true;
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) return true;
        return fn(line.substr(0, eq), line.substr(eq + 1));
    });
}

// Scope-exit helper: run_finally fin([]{ ... });
template <class F> struct run_finally {
    F fn;
    explicit run_finally(F f) : fn(std::move(f)) {}
    ~run_finally() { fn(); }
    run_finally(const run_finally &) = delete;
    run_finally &operator=(const run_finally &) = delete;
};
template <class F> run_finally(F) -> run_finally<F>;
