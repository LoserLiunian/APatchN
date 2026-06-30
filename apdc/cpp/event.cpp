#include "event.hpp"

#include "assets.hpp"
#include "logging.hpp"
#include "lua.hpp"
#include "metamodule.hpp"
#include "module.hpp"
#include "module_config.hpp"
#include "process.hpp"
#include "restorecon.hpp"
#include "result.hpp"
#include "sepolicy.hpp"
#include "supercall.hpp"
#include "utils.hpp"

#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace apd::event {
namespace {

std::string superkey_or_su(const OptStr &k) { return k ? *k : std::string("su"); }

bool path_exists(const fs::path &p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

template <class F> void warn_on_throw(const char *what, F &&f) {
    try {
        f();
    } catch (const std::exception &e) {
        log::warn("{}: {}", what, e.what());
    }
}

} // namespace

void report_kernel(const OptStr &superkey, const char *event, const char *state) {
    // truncate is SUPERCMD, intercepted by the kernel.
    proc::Cmd cmd;
    cmd.program = "truncate";
    cmd.args = {superkey_or_su(superkey), "event", event, state};
    proc::run(cmd);
}

static void run_stage(const std::string &stage, const OptStr &superkey, bool block) {
    utils::umask(0);

    if (utils::has_magisk()) {
        log::warn("Magisk detected, skip {}", stage);
        return;
    }
    if (utils::is_safe_mode(superkey)) {
        log::warn("safe mode, skip {} scripts", stage);
        warn_on_throw("disable all modules failed", [] { module::disable_all_modules(); });
        return;
    }

    warn_on_throw("Failed to exec metamodule stage script",
                  [&] { metamodule::exec_stage_script(stage, block); });
    warn_on_throw("Failed to exec common scripts",
                  [&] { module::exec_common_scripts(stage + ".d", block); });
    warn_on_throw("Failed to exec stage scripts",
                  [&] { module::exec_stage_script(stage, block); });
    warn_on_throw("Failed to exec stage lua",
                  [&] { lua::exec_stage_lua(stage, block, superkey ? *superkey : std::string()); });
}

void on_post_data_fs(const OptStr &superkey) {
    utils::umask(0);
    report_kernel(superkey, "post-fs-data", "before");

    supercall::init_load_su_path(superkey);

    // inject Magisk sepolicy rules and load into the kernel
    sepolicy::apply_magisk_rules_live();

    log::info("Re-privilege apd profile after injecting sepolicy");
    supercall::privilege_apd_profile(superkey);

    warn_on_throw("clear temp configs failed", [] { module_config::clear_all_temp_configs(); });

    if (utils::has_magisk()) {
        log::warn("Magisk detected, skip post-fs-data!");
        report_kernel(superkey, "post-fs-data", "after");
        return;
    }

    // log environment
    if (!path_exists(defs::APATCH_LOG_FOLDER)) {
        std::error_code ec;
        fs::create_directory(defs::APATCH_LOG_FOLDER, ec);
        ::chmod(defs::APATCH_LOG_FOLDER, 0700);
    }
    {
        std::string log = defs::APATCH_LOG_FOLDER;
        std::string c = "rm -rf " + log + "*.old.log; for file in " + log +
                        "*; do mv \"$file\" \"$file.old.log\"; done";
        proc::Cmd sh;
        sh.program = "sh";
        sh.args = {"-c", c};
        if (proc::run(sh)) log::info("Successfully deleted .old files.");
        else log::info("Failed to delete .old files.");
    }

    std::string logcat_path = std::string(defs::APATCH_LOG_FOLDER) + "logcat.log";
    std::string dmesg_path = std::string(defs::APATCH_LOG_FOLDER) + "dmesg.log";

    {
        proc::Cmd logcat;
        logcat.program = "timeout";
        logcat.new_process_group = true;
        logcat.child_switch_cgroups = true;
        logcat.args = {"-s",  "9",        "45s",  "logcat", "-b", "main,system,crash",
                       "DrmLibFs:S", "-f", logcat_path, "logcatcher-bootlog:S", "&"};
        try {
            proc::spawn(logcat);
        } catch (const std::exception &) {
        }
    }
    {
        int bootlog = ::open(dmesg_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        proc::Cmd dmesg;
        dmesg.program = "timeout";
        dmesg.new_process_group = true;
        dmesg.child_switch_cgroups = true;
        dmesg.args = {"-s", "9", "120s", "dmesg", "-w"};
        dmesg.stdout_fd = bootlog;
        try {
            proc::spawn(dmesg);
        } catch (const std::exception &) {
        }
        if (bootlog >= 0) ::close(bootlog);
    }

    for (const char *key : {"KERNELPATCH_VERSION", "KERNEL_VERSION"}) {
        const char *v = std::getenv(key);
        if (v) std::printf("%s: %s\n", key, v);
        else std::printf("%s not found\n", key);
    }

    bool safe_mode = utils::is_safe_mode(superkey);
    if (safe_mode) {
        log::warn("safe mode, skip common post-fs-data.d scripts");
        warn_on_throw("disable all modules failed", [] { module::disable_all_modules(); });
    } else {
        warn_on_throw("exec common post-fs-data scripts failed",
                      [] { module::exec_common_scripts("post-fs-data.d", true); });
    }

    fs::path module_update_flag = fs::path(defs::WORKING_DIR) / defs::UPDATE_FILE_NAME;
    assets::ensure_binaries();

    if (path_exists(defs::MODULE_UPDATE_DIR)) {
        module::handle_updated_modules();
        std::error_code ec;
        fs::remove_all(defs::MODULE_UPDATE_DIR, ec);
    }

    if (safe_mode) {
        log::warn("safe mode, skip post-fs-data scripts and disable all modules!");
        warn_on_throw("disable all modules failed", [] { module::disable_all_modules(); });
        return;
    }

    warn_on_throw("prune modules failed", [] { module::prune_modules(); });
    warn_on_throw("restorecon failed", [] { restorecon::restorecon(); });
    warn_on_throw("load sepolicy.rule failed", [] { module::load_sepolicy_rule(); });
    warn_on_throw("execute metamodule mount failed",
                  [] { metamodule::exec_mount_script(defs::MODULE_DIR); });
    warn_on_throw("exec post-fs-data scripts failed",
                  [] { module::exec_stage_script("post-fs-data", true); });
    warn_on_throw("Failed to exec post-fs-data lua",
                  [&] { lua::exec_stage_lua("post-fs-data", true, superkey ? *superkey : ""); });
    warn_on_throw("load system.prop failed", [] { module::load_system_prop(); });

    log::info("remove update flag");
    {
        std::error_code ec;
        fs::remove(module_update_flag, ec);
    }

    run_stage("post-mount", superkey, true);

    if (::chdir("/") != 0) bail("failed to chdir to /");
    report_kernel(superkey, "post-fs-data", "after");
}

void on_services(const OptStr &superkey) {
    log::info("on_services triggered!");
    run_stage("service", superkey, false);
}

static void run_uid_monitor() {
    log::info("Trigger run_uid_monitor!");
    proc::Cmd cmd;
    cmd.program = "/data/adb/apd";
    cmd.args = {"uid-listener"};
    cmd.new_process_group = true;
    cmd.child_switch_cgroups = true;
    proc::spawn(cmd);
}

void on_boot_completed(const OptStr &superkey) {
    log::info("on_boot_completed triggered!");
    run_stage("boot-completed", superkey, false);
    run_uid_monitor();
}

// --- uid listener ---------------------------------------------------------

namespace {

class BoolQueue {
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<bool> q_;

public:
    void push(bool v) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(v);
        }
        cv_.notify_one();
    }
    bool pop() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return !q_.empty(); });
        bool v = q_.front();
        q_.pop_front();
        return v;
    }
};

} // namespace

void start_uid_listener() {
    log::info("start_uid_listener triggered!");
    std::printf("[start_uid_listener] Registering...\n");

    static std::mutex refresh_mtx;
    static BoolQueue queue;
    const std::string watch_dir = "/data/system";
    const std::string tmp_name = "packages.list.tmp";

    // shutdown signals -> refresh once
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGPWR);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    std::thread([set]() {
        int sig = 0;
        sigset_t s = set;
        if (sigwait(&s, &sig) == 0) {
            log::warn("[shutdown] Caught signal {}, refreshing package list...", sig);
            supercall::refresh_ap_package_list("su", refresh_mtx);
        }
    }).detach();

    int fd = ::inotify_init1(IN_CLOEXEC);
    if (fd < 0) bail("inotify_init failed");
    if (::inotify_add_watch(fd, watch_dir.c_str(), IN_MOVED_FROM | IN_MOVED_TO) < 0)
        bail("inotify_add_watch failed");

    std::thread([fd, tmp_name]() {
        alignas(struct inotify_event) char buf[4096];
        for (;;) {
            ssize_t len = ::read(fd, buf, sizeof buf);
            if (len < 0) {
                // EINTR (signal from sigwait reaper thread, SIGCHLD on spawn, etc.)
                // must not kill the uid-listener loop. Rust's notify-rs retries
                // transparently; mirror that. Bail only on permanent errors.
                if (errno == EINTR || errno == EAGAIN) continue;
                log::error("[uid_monitor] inotify read failed: errno={}", errno);
                break;
            }
            if (len == 0) break;
            for (char *p = buf; p < buf + len;) {
                auto *e = reinterpret_cast<struct inotify_event *>(p);
                if (e->len > 0 && (e->mask & (IN_MOVED_FROM | IN_MOVED_TO)) &&
                    tmp_name == e->name) {
                    log::info("[uid_monitor] System packages list changed, sending to tx...");
                    queue.push(false);
                }
                p += sizeof(struct inotify_event) + e->len;
            }
        }
    }).detach();

    bool debounce = false;
    for (;;) {
        bool delayed = queue.pop();
        if (delayed) {
            debounce = false;
            supercall::refresh_ap_package_list("su", refresh_mtx);
            warn_on_throw("Failed to report kernel about package list update", [] {
                report_kernel(std::nullopt, "uid_listener", "package-list-updated");
            });
        } else if (!debounce) {
            ::sleep(1);
            debounce = true;
            queue.push(true);
        }
    }
}

} // namespace apd::event
