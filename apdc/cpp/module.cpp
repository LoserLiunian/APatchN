#include "module.hpp"

#include "assets.hpp"
#include "embedded_scripts.hpp"
#include "json.hpp"
#include "logging.hpp"
#include "lua.hpp"
#include "metamodule.hpp"
#include "module_config.hpp"
#include "process.hpp"
#include "properties.hpp"
#include "resetprop.hpp"
#include "restorecon.hpp"
#include "result.hpp"
#include "sepolicy.hpp"
#include "utils.hpp"
#include "zip_util.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace apd::module {
namespace {

std::string installer_content() { return INSTALLER_SH; }
std::string install_module_script() {
    return std::string(INSTALLER_SH) + "\ninstall_module\nexit 0\n";
}

std::string trim(const std::string &s) {
    // Match Rust str::trim's ASCII-whitespace set: ' ', '\t', '\n', '\v' (0x0B),
    // '\f' (0x0C), '\r'. Missing \f/\v would let a module.prop id with these
    // bytes slip past the metamodule duplicate-id check.
    static constexpr const char *kWs = " \t\n\v\f\r";
    auto b = s.find_first_not_of(kWs);
    auto e = s.find_last_not_of(kWs);
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

bool path_exists(const fs::path &p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool is_executable(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && ::access(p.string().c_str(), X_OK) == 0;
}

void ensure_boot_completed() {
    if (utils::getprop("sys.boot_completed") != OptStr("1")) bail("Android is Booting!");
}

void mark_update() {
    utils::ensure_file_exists(std::string(defs::WORKING_DIR) + defs::UPDATE_FILE_NAME);
}

void mark_module_state(const std::string &module, const char *flag_file, bool create) {
    fs::path file = fs::path(defs::MODULE_DIR) / module / flag_file;
    if (create) {
        // Rust tolerates a missing parent dir here (uninstall of a module that's
        // already gone from /data/adb/modules/): touching the flag is a best-
        // effort hint. Don't throw — log and move on.
        std::error_code ec;
        if (!fs::exists(file.parent_path(), ec)) {
            log::debug("mark_module_state: parent missing for {}, skipping", file.string());
            return;
        }
        try {
            utils::ensure_file_exists(file.string());
        } catch (const std::exception &e) {
            log::warn("mark_module_state: create({}) failed: {}", file.string(), e.what());
        }
    } else if (path_exists(file)) {
        std::error_code ec;
        fs::remove(file, ec);
        // Rust silently ignores the remove error (matches Magisk behavior); do
        // the same here so `apd module enable foo` does not surface EPERM/EACCES.
        // Log so it's observable in logcat.
        if (ec) log::warn("mark_module_state: remove({}) failed: {}", file.string(), ec.message());
    }
}

void foreach_active_module(const std::function<void(const fs::path &)> &f) {
    foreach_module(ModuleType::Active, f);
}

void exec_install_script(const std::string &module_file, bool is_metamodule,
                         const std::string &module_id) {
    std::error_code ec;
    fs::path realpath = fs::canonical(module_file, ec);
    if (ec) bailf("realpath: {} failed", module_file);

    std::string script = metamodule::get_install_script(is_metamodule, installer_content(),
                                                        install_module_script());
    proc::Cmd cmd;
    cmd.program = assets::BUSYBOX_PATH;
    cmd.args = {"sh", "-c", script};
    cmd.env = get_common_script_envs(module_id);
    cmd.env.emplace_back("OUTFD", "1");
    cmd.env.emplace_back("ZIPFILE", realpath.string());
    ensure(proc::run(cmd), "Failed to install module script");
}

void _install_module(const std::string &zip) {
    ensure_boot_completed();

    std::printf("%s\n", BANNER);

    assets::ensure_binaries();
    utils::ensure_dir_exists(defs::WORKING_DIR);
    utils::ensure_dir_exists(defs::BINARY_DIR);

    std::error_code ec;
    fs::path zip_path = fs::canonical(zip, ec);
    if (ec) bailf("realpath: {} failed", zip);

    // read module_id from module.prop inside the zip
    std::string buf = zip::extract_file_to_memory(zip_path.string(), "module.prop");
    auto module_prop = properties::parse(buf);
    auto id_it = module_prop.find("id");
    if (id_it == module_prop.end()) bail("module id not found in module.prop!");
    std::string module_id = trim(id_it->second);

    bool is_metamodule = metamodule::is_metamodule(module_prop);

    // needs_mount: has system/ and no skip_mount
    bool has_system = false, has_skip_mount = false;
    for (const auto &name : zip::list_names(zip_path.string())) {
        if (name.rfind("system/", 0) == 0) has_system = true;
        if (name == "skip_mount") has_skip_mount = true;
    }
    bool needs_mount = has_system && !has_skip_mount;

    if (!is_metamodule && needs_mount) {
        auto safety = metamodule::check_install_safety();
        if (!safety.safe) {
            std::printf("\n❌ Installation Blocked\n");
            std::printf("┌────────────────\n");
            std::printf("│ A metamodule with custom installer is active\n");
            std::printf("│\n");
            if (safety.blocked_disabled) {
                std::printf("│ Current state: Disabled\n");
                std::printf("│ Action required: Re-enable or uninstall it, then reboot\n");
            } else {
                std::printf("│ Current state: Pending changes\n");
                std::printf("│ Action required: Reboot to apply changes first\n");
            }
            std::printf("└─────────────────\n\n");
            bail("Metamodule installation blocked");
        }
    }

    if (!path_exists(defs::MODULE_DIR)) {
        fs::create_directory(defs::MODULE_DIR, ec);
        ::chmod(defs::MODULE_DIR, 0700);
    }

    if (is_metamodule) {
        log::info("Installing metamodule: {}", module_id);
        if (metamodule::has_metamodule()) {
            auto existing_path = metamodule::get_metamodule_path();
            if (existing_path) {
                std::string existing_id = "unknown";
                try {
                    auto m = read_module_prop(*existing_path);
                    if (auto it = m.find("id"); it != m.end()) existing_id = it->second;
                } catch (const std::exception &) {
                }
                if (existing_id != module_id) {
                    std::printf("\n❌ Installation Failed\n");
                    std::printf("│ A metamodule is already installed\n");
                    std::printf("│   Current metamodule: %s\n", existing_id.c_str());
                    std::printf("│ Only one metamodule can be active at a time.\n");
                    bail("Cannot install multiple metamodules");
                }
            }
        }
    }

    std::string module_dir = std::string(defs::MODULE_DIR) + module_id;
    std::string module_update_dir = std::string(defs::MODULE_UPDATE_DIR) + module_id;
    log::info("module dir: {}", module_dir);
    if (!path_exists(module_dir)) {
        fs::create_directory(module_dir, ec);
        ::chmod(module_dir.c_str(), 0700);
    }

    // unzip into modules_update/<id>
    zip::extract_all(zip_path.string(), module_update_dir);

    std::printf("- Running module installer\n");
    exec_install_script(zip_path.string(), is_metamodule, module_id);

    fs::path module_system_dir = fs::path(module_dir) / "system";
    if (path_exists(module_system_dir)) {
        ::chmod(module_system_dir.string().c_str(), 0755);
        restorecon::restore_syscon(module_system_dir.string());
    }

    if (is_metamodule) {
        std::printf("- Creating metamodule symlink\n");
        metamodule::ensure_symlink(module_dir);
    }

    mark_update();
}

void _uninstall_module(const std::string &id, const std::string &update_dir) {
    ensure(path_exists(update_dir), "No module installed");

    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(update_dir, ec)) {
        fs::path module_prop = entry.path() / "module.prop";
        if (!path_exists(module_prop)) continue;
        auto props = properties::parse_file(module_prop.string());
        auto it = props.find("id");
        if (it != props.end() && it->second == id) {
            fs::path remove_file = entry.path() / defs::REMOVE_FILE_NAME;
            std::ofstream(remove_file).flush();
            if (!path_exists(remove_file)) bail("Failed to create remove file.");
            break;
        }
    }

    fs::path target_module = fs::path(update_dir) / id;
    if (path_exists(target_module)) {
        fs::path remove_file = target_module / defs::REMOVE_FILE_NAME;
        if (!path_exists(remove_file)) {
            std::ofstream(remove_file).flush();
            if (!path_exists(remove_file)) bail("Failed to create remove file.");
        }
    }

    mark_module_state(id, defs::REMOVE_FILE_NAME, true);
}

void _undo_uninstall_module(const std::string &id, const std::string &update_dir) {
    ensure(path_exists(update_dir), "No module installed");

    std::error_code ec;
    bool found = false;
    for (const auto &entry : fs::directory_iterator(update_dir, ec)) {
        fs::path module_prop = entry.path() / "module.prop";
        if (!path_exists(module_prop)) continue;
        auto props = properties::parse_file(module_prop.string());
        auto it = props.find("id");
        if (it != props.end() && it->second == id) {
            fs::path remove_file = entry.path() / defs::REMOVE_FILE_NAME;
            if (!fs::remove(remove_file, ec)) bail("Failed to remove removefile.");
            found = true;
            break;
        }
    }
    ensure(found, "Module not found");
    mark_module_state(id, defs::REMOVE_FILE_NAME, false);
}

void _change_module_state(const std::string &module_dir, const std::string &mid, bool enable) {
    fs::path src_module = fs::path(module_dir) / mid;
    ensuref(path_exists(src_module), "module: {} not found!", mid);

    fs::path disable_path = src_module / defs::DISABLE_FILE_NAME;
    std::error_code ec;
    if (enable) {
        if (path_exists(disable_path) && !fs::remove(disable_path, ec))
            bailf("Failed to remove disable file: {}", disable_path.string());
    } else {
        utils::ensure_file_exists(disable_path.string());
    }
    mark_module_state(mid, defs::DISABLE_FILE_NAME, !enable);
}

void _disable_all_modules(const std::string &dir) {
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        fs::path disable_flag = entry.path() / defs::DISABLE_FILE_NAME;
        try {
            utils::ensure_file_exists(disable_flag.string());
        } catch (const std::exception &e) {
            log::warn("Failed to disable module: {}: {}", entry.path().string(), e.what());
        }
    }
}

std::vector<std::map<std::string, std::string>> _list_modules(const std::string &path) {
    std::map<std::string, module_config::Config> all_configs;
    try {
        all_configs = module_config::get_all_module_configs();
    } catch (const std::exception &e) {
        log::warn("Failed to load module configs: {}", e.what());
    }

    std::vector<std::map<std::string, std::string>> modules;
    std::error_code ec;
    if (!fs::exists(path, ec)) return modules;

    for (const auto &entry : fs::directory_iterator(path, ec)) {
        fs::path p = entry.path();
        log::info("path: {}", p.string());
        fs::path module_prop = p / "module.prop";
        if (!path_exists(module_prop)) continue;

        std::map<std::string, std::string> prop;
        try {
            prop = properties::parse_file(module_prop.string());
        } catch (const std::exception &) {
            log::warn("Failed to parse module.prop: {}", module_prop.string());
            continue;
        }

        if (prop.find("id") == prop.end() || prop["id"].empty()) {
            std::string dir_name = p.filename().string();
            if (!dir_name.empty()) {
                log::info("Use dir name as module id: {}", dir_name);
                prop["id"] = dir_name;
            } else {
                continue;
            }
        }

        std::string id = prop["id"];
        bool enabled = !path_exists(p / defs::DISABLE_FILE_NAME);
        bool update = path_exists(p / defs::UPDATE_FILE_NAME);
        bool remove = path_exists(p / defs::REMOVE_FILE_NAME);
        bool web = path_exists(p / defs::MODULE_WEB_DIR);
        bool action = path_exists(p / defs::MODULE_ACTION_SH) || path_exists(p / (id + ".lua"));

        prop["enabled"] = enabled ? "true" : "false";
        prop["update"] = update ? "true" : "false";
        prop["remove"] = remove ? "true" : "false";
        prop["web"] = web ? "true" : "false";
        prop["action"] = action ? "true" : "false";

        if (auto cit = all_configs.find(id); cit != all_configs.end()) {
            auto dit = cit->second.find("override.description");
            if (dit != cit->second.end()) prop["description"] = dit->second;
        }

        modules.push_back(std::move(prop));
    }
    return modules;
}

} // namespace

std::vector<std::pair<std::string, std::string>> get_common_script_envs(const OptStr &module_id) {
    const char *cur_path = std::getenv("PATH");
    std::string bin = defs::BINARY_DIR;
    while (!bin.empty() && bin.back() == '/') bin.pop_back();
    std::string path_env = (cur_path ? std::string(cur_path) : std::string()) + ":" + bin;

    std::vector<std::pair<std::string, std::string>> envs = {
        {"ASH_STANDALONE", "1"},
        {"APATCH", "true"},
        {"APATCH_VER", defs::VERSION_NAME},
        {"APATCH_VER_CODE", defs::VERSION_CODE},
        {"PATH", path_env},
    };
    if (module_id) envs.emplace_back("AP_MODULE", *module_id);
    return envs;
}

std::map<std::string, std::string> read_module_prop(const fs::path &module_path) {
    fs::path module_prop = module_path / "module.prop";
    ensuref(path_exists(module_prop), "module.prop not found in {}", module_path.string());
    return properties::parse_file(module_prop.string());
}

void foreach_module(ModuleType type, const std::function<void(const fs::path &)> &f) {
    const char *dir = (type == ModuleType::Updated) ? defs::MODULE_UPDATE_DIR : defs::MODULE_DIR;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        fs::path path = entry.path();
        if (!fs::is_directory(path, ec)) {
            log::warn("{} is not a directory, skip", path.string());
            continue;
        }
        if (type == ModuleType::Active && path_exists(path / defs::DISABLE_FILE_NAME)) {
            log::info("{} is disabled, skip", path.string());
            continue;
        }
        if (type == ModuleType::Active && path_exists(path / defs::REMOVE_FILE_NAME)) {
            log::warn("{} is removed, skip", path.string());
            continue;
        }
        f(path);
    }
}

void exec_script(const fs::path &path, bool wait) {
    log::info("exec {}", path.string());

    OptStr module_id;
    std::string ps = path.string();
    std::string prefix = defs::MODULE_DIR;
    if (ps.rfind(prefix, 0) == 0) {
        std::string rel = ps.substr(prefix.size());
        std::size_t slash = rel.find('/');
        std::string first = (slash == std::string::npos) ? rel : rel.substr(0, slash);
        if (!first.empty()) module_id = first;
    }

    proc::Cmd cmd;
    cmd.program = assets::BUSYBOX_PATH;
    cmd.new_process_group = true;
    cmd.child_switch_cgroups = true;
    cmd.cwd = path.parent_path().string();
    cmd.args = {"sh", path.string()};
    cmd.env = get_common_script_envs(module_id);

    try {
        pid_t pid = proc::spawn(cmd);
        if (wait) proc::wait_pid(pid);
    } catch (const std::exception &e) {
        bailf("Failed to exec {}: {}", path.string(), e.what());
    }
}

void exec_stage_script(const std::string &stage, bool block) {
    foreach_active_module([&](const fs::path &module) {
        fs::path script_path = module / (stage + ".sh");
        if (!path_exists(script_path)) return;
        exec_script(script_path, block);
    });
}

void exec_common_scripts(const std::string &dir, bool wait) {
    fs::path script_dir = fs::path(defs::ADB_DIR) / dir;
    if (!path_exists(script_dir)) {
        log::info("{} not exists, skip", script_dir.string());
        return;
    }
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(script_dir, ec)) {
        fs::path path = entry.path();
        if (!is_executable(path)) {
            log::warn("{} is not executable, skip", path.string());
            continue;
        }
        exec_script(path, wait);
    }
}

void load_system_prop() {
    foreach_active_module([](const fs::path &module) {
        fs::path system_prop = module / "system.prop";
        if (!path_exists(system_prop)) return;
        log::info("load {} system.prop", module.string());
        resetprop::load_system_prop_file(system_prop.string());
    });
}

void handle_updated_modules() {
    fs::path modules_root(defs::MODULE_DIR);
    foreach_module(ModuleType::Updated, [&](const fs::path &updated_module) {
        std::error_code ec;
        if (!fs::is_directory(updated_module, ec)) return;
        std::string name = updated_module.filename().string();
        if (name.empty()) return;

        fs::path module_dir = modules_root / name;
        bool disabled = false, removed = false;
        if (path_exists(module_dir)) {
            disabled = path_exists(module_dir / defs::DISABLE_FILE_NAME);
            removed = path_exists(module_dir / defs::REMOVE_FILE_NAME);
            fs::remove_all(module_dir, ec);
        }
        fs::rename(updated_module, module_dir, ec);
        if (ec) bailf("Failed to move updated module {}", name);

        if (removed) {
            try {
                utils::ensure_file_exists((module_dir / defs::REMOVE_FILE_NAME).string());
            } catch (const std::exception &e) {
                log::warn("Failed to create remove flag: {}", e.what());
            }
        } else if (disabled) {
            try {
                utils::ensure_file_exists((module_dir / defs::DISABLE_FILE_NAME).string());
            } catch (const std::exception &e) {
                log::warn("Failed to create disable flag: {}", e.what());
            }
        }
    });
}

void prune_modules() {
    foreach_module(ModuleType::All, [](const fs::path &module) {
        std::error_code ec;
        fs::remove(module / defs::UPDATE_FILE_NAME, ec);
        if (!path_exists(module / defs::REMOVE_FILE_NAME)) return;

        log::info("remove module: {}", module.string());
        std::string module_id = module.filename().string();

        bool is_meta = false;
        try {
            is_meta = metamodule::is_metamodule(read_module_prop(module));
        } catch (const std::exception &) {
        }

        if (is_meta) {
            log::info("Removing metamodule symlink");
            try {
                metamodule::remove_symlink();
            } catch (const std::exception &e) {
                log::warn("Failed to remove metamodule symlink: {}", e.what());
            }
        } else {
            try {
                metamodule::exec_metauninstall_script(module_id);
            } catch (const std::exception &e) {
                log::warn("Failed to exec metamodule uninstall for {}: {}", module_id, e.what());
            }
        }

        fs::path uninstaller = module / "uninstall.sh";
        if (path_exists(uninstaller)) {
            try {
                exec_script(uninstaller, true);
            } catch (const std::exception &e) {
                log::warn("Failed to exec uninstaller: {}", e.what());
            }
        }

        try {
            module_config::clear_module_configs(module_id);
        } catch (const std::exception &e) {
            log::warn("Failed to clear configs for {}: {}", module_id, e.what());
        }

        fs::remove_all(module, ec);
        if (ec) log::warn("Failed to remove {}", module.string());
    });

    std::error_code ec;
    bool any = false;
    for (const auto &entry : fs::directory_iterator(defs::MODULE_DIR, ec)) {
        if (path_exists(entry.path() / "module.prop")) {
            any = true;
            break;
        }
    }
    if (!any) log::info("no remaining modules.");
}

void load_sepolicy_rule() {
    foreach_active_module([](const fs::path &path) {
        fs::path rule_file = path / "sepolicy.rule";
        if (!path_exists(rule_file)) return;
        log::info("load policy: {}", rule_file.string());
        sepolicy::apply_rule_file_live(rule_file.string());
    });
}

void disable_all_modules() {
    if (utils::getprop("sys.boot_completed") == OptStr("1")) {
        log::info("System boot completed, no need to disable all modules");
        return;
    }
    mark_update();
    _disable_all_modules(defs::MODULE_DIR);
}

void install_module(const std::string &zip) { _install_module(zip); }

void uninstall_module(const std::string &id) {
    _uninstall_module(id, defs::MODULE_DIR);
    mark_update();
}

void undo_uninstall_module(const std::string &id) {
    _undo_uninstall_module(id, defs::MODULE_DIR);
    mark_update();
}

void enable_module(const std::string &id) { _change_module_state(defs::MODULE_DIR, id, true); }
void disable_module(const std::string &id) { _change_module_state(defs::MODULE_DIR, id, false); }

void run_action(const std::string &id) {
    std::string action_script = std::string("/data/adb/modules/") + id + "/action.sh";
    if (path_exists(action_script)) {
        try {
            exec_script(action_script, true);
        } catch (const std::exception &) {
        }
    } else {
        lua::run_lua(id, "action", false, true);
    }
}

void list_modules() {
    auto modules = _list_modules(defs::MODULE_DIR);
    std::printf("%s\n", json::to_pretty(modules).c_str());
}

} // namespace apd::module
