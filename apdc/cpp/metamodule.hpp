#pragma once
// Metamodule management. Mirrors ../apd/src/metamodule.rs.
#include <map>
#include <optional>
#include <string>

namespace apd::metamodule {

bool is_metamodule(const std::map<std::string, std::string> &props);

// Resolved metamodule directory (via /data/adb/metamodule symlink, else scan).
std::optional<std::string> get_metamodule_path();
std::optional<std::string> get_metamodule_id();
bool has_metamodule();

// Result of the pre-install safety check. `safe==false` blocks installation;
// `blocked_disabled` distinguishes a disabled metamodule from other unstable states.
struct InstallSafety {
    bool safe;
    bool blocked_disabled;
};
InstallSafety check_install_safety();

void ensure_symlink(const std::string &module_path);
void remove_symlink();

std::string get_install_script(bool is_metamodule, const std::string &installer_content,
                               const std::string &install_module_script);

void exec_metauninstall_script(const std::string &module_id);
void exec_mount_script(const std::string &module_dir);
void exec_stage_script(const std::string &stage, bool block);

} // namespace apd::metamodule
