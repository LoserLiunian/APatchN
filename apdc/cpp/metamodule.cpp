#include "metamodule.hpp"

#include "assets.hpp"
#include "defs.hpp"
#include "logging.hpp"
#include "module.hpp"
#include "process.hpp"
#include "result.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace apd::metamodule {
namespace {

std::string symlink_path() {
    // METAMODULE_DIR has a trailing slash; trim it for symlink operations.
    std::string p = defs::METAMODULE_DIR;
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

std::string read_to_string(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::optional<std::string> get_metamodule_id_of(const std::string &path) {
    fs::path p(path);
    auto name = p.filename();
    if (name.empty()) return std::nullopt;
    return name.string();
}

} // namespace

bool is_metamodule(const std::map<std::string, std::string> &props) {
    auto it = props.find("metamodule");
    if (it == props.end()) return false;
    std::string s = it->second;
    auto b = s.find_first_not_of(" \t\n\v\f\r");
    auto e = s.find_last_not_of(" \t\n\v\f\r");
    std::string t = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
    if (t == "1") return true;
    std::string lower;
    for (char c : t) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return lower == "true";
}

std::optional<std::string> get_metamodule_path() {
    fs::path link(symlink_path());
    std::error_code ec;

    if (fs::is_symlink(link, ec)) {
        fs::path target = fs::read_symlink(link, ec);
        if (!ec) {
            fs::path resolved = target.is_absolute() ? target : link.parent_path() / target;
            if (fs::exists(resolved, ec) && fs::is_directory(resolved, ec))
                return resolved.string();
            log::warn("Metamodule symlink points to non-existent path: {}", resolved.string());
        }
    }

    // Fallback: scan modules for metamodule=1 (last match wins, mirrors Rust).
    std::optional<std::string> result;
    module::foreach_module(module::ModuleType::All, [&](const fs::path &module_path) {
        try {
            auto props = module::read_module_prop(module_path);
            if (is_metamodule(props)) {
                log::info("Found metamodule in modules directory: {}", module_path.string());
                result = module_path.string();
            }
        } catch (const std::exception &) {
        }
    });
    return result;
}

std::optional<std::string> get_metamodule_id() {
    auto path = get_metamodule_path();
    if (!path) return std::nullopt;
    return get_metamodule_id_of(*path);
}

bool has_metamodule() { return get_metamodule_path().has_value(); }

InstallSafety check_install_safety() {
    auto mp = get_metamodule_path();
    if (!mp) return {true, false};
    fs::path metamodule_path(*mp);
    std::error_code ec;

    bool has_metainstall = fs::exists(metamodule_path / defs::METAMODULE_METAINSTALL_SCRIPT, ec);
    if (!has_metainstall) {
        auto id = get_metamodule_id_of(*mp);
        if (id)
            has_metainstall = fs::exists(fs::path(defs::MODULE_UPDATE_DIR) / *id /
                                             defs::METAMODULE_METAINSTALL_SCRIPT,
                                         ec);
    }
    if (!has_metainstall) return {true, false};

    bool has_update = fs::exists(metamodule_path / defs::UPDATE_FILE_NAME, ec);
    bool has_remove = fs::exists(metamodule_path / defs::REMOVE_FILE_NAME, ec);
    bool has_disable = fs::exists(metamodule_path / defs::DISABLE_FILE_NAME, ec);

    if (!has_update && !has_remove && !has_disable) return {true, false};

    return {false, has_disable && !has_update && !has_remove};
}

void ensure_symlink(const std::string &module_path) {
    fs::path link(symlink_path());
    std::error_code ec;

    log::info("Creating metamodule symlink: {} -> {}", link.string(), module_path);

    if (fs::exists(link, ec) || fs::is_symlink(link, ec)) {
        log::info("Removing old metamodule symlink/path");
        if (fs::is_symlink(link, ec)) {
            if (!fs::remove(link, ec)) bail("Failed to remove old symlink");
        } else {
            fs::remove_all(link, ec);
            if (ec) bail("Failed to remove old directory");
        }
    }

    if (::symlink(module_path.c_str(), link.string().c_str()) != 0)
        bailf("Failed to create symlink to {}", module_path);
    log::info("Metamodule symlink created successfully");
}

void remove_symlink() {
    fs::path link(symlink_path());
    std::error_code ec;
    if (fs::is_symlink(link, ec)) {
        if (!fs::remove(link, ec)) bail("Failed to remove metamodule symlink");
        log::info("Metamodule symlink removed");
    }
}

std::string get_install_script(bool is_meta, const std::string &installer_content,
                               const std::string &install_module_script) {
    if (is_meta) {
        log::info("Installing metamodule, using default installer");
        return install_module_script;
    }
    auto mp = get_metamodule_path();
    if (!mp) {
        log::info("No metamodule found, using default installer");
        return install_module_script;
    }
    fs::path metamodule_path(*mp);
    std::error_code ec;
    if (fs::exists(metamodule_path / defs::DISABLE_FILE_NAME, ec)) {
        log::info("Metamodule is disabled, using default installer");
        return install_module_script;
    }
    fs::path metainstall = metamodule_path / defs::METAMODULE_METAINSTALL_SCRIPT;
    if (fs::exists(metainstall, ec)) {
        log::info("Using metainstall.sh from metamodule");
        std::string content = read_to_string(metainstall);
        return installer_content + "\n" + content + "\nexit 0\n";
    }
    log::info("Metamodule exists but has no metainstall.sh, using default installer");
    return install_module_script;
}

// Returns the script path if the metamodule exists, is enabled, and has it.
static std::optional<fs::path> check_metamodule_script(const std::string &script_name) {
    auto mp = get_metamodule_path();
    if (!mp) return std::nullopt;
    fs::path metamodule_path(*mp);
    std::error_code ec;
    if (fs::exists(metamodule_path / defs::DISABLE_FILE_NAME, ec)) {
        log::info("Metamodule is disabled, skipping {}", script_name);
        return std::nullopt;
    }
    fs::path script = metamodule_path / script_name;
    if (!fs::exists(script, ec)) return std::nullopt;
    return script;
}

void exec_metauninstall_script(const std::string &module_id) {
    auto script = check_metamodule_script(defs::METAMODULE_METAUNINSTALL_SCRIPT);
    if (!script) return;

    log::info("Executing metamodule metauninstall.sh for module: {}", module_id);
    proc::Cmd cmd;
    cmd.program = assets::BUSYBOX_PATH;
    cmd.args = {"sh", script->string()};
    cmd.cwd = script->parent_path().string();
    cmd.env = module::get_common_script_envs(get_metamodule_id());
    cmd.env.emplace_back("MODULE_ID", module_id);
    ensuref(proc::run(cmd), "Metamodule metauninstall.sh failed for module {}", module_id);
    log::info("Metamodule metauninstall.sh executed successfully for {}", module_id);
}

void exec_mount_script(const std::string &module_dir) {
    auto script = check_metamodule_script(defs::METAMODULE_MOUNT_SCRIPT);
    if (!script) return;

    log::info("Executing mount script for metamodule");
    proc::Cmd cmd;
    cmd.program = assets::BUSYBOX_PATH;
    cmd.args = {"sh", script->string()};
    cmd.env = module::get_common_script_envs(get_metamodule_id());
    cmd.env.emplace_back("MODULE_DIR", module_dir);
    ensure(proc::run(cmd), "Metamodule mount script failed");
    log::info("Metamodule mount script executed successfully");
}

void exec_stage_script(const std::string &stage, bool block) {
    auto script = check_metamodule_script(stage + ".sh");
    if (!script) return;
    log::info("Executing metamodule {}.sh", stage);
    module::exec_script(*script, block);
    log::info("Metamodule {}.sh executed successfully", stage);
}

} // namespace apd::metamodule
