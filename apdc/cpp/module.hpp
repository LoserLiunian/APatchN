#pragma once
// APM module management. Mirrors ../apd/src/module.rs.
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "defs.hpp"

namespace apd::module {

enum class ModuleType { All, Active, Updated };

// CLI operations
void install_module(const std::string &zip);
void uninstall_module(const std::string &id);
void undo_uninstall_module(const std::string &id);
void enable_module(const std::string &id);
void disable_module(const std::string &id);
void run_action(const std::string &id);
void list_modules();

// Boot-stage helpers (driven by event.rs, phase 3) + metamodule reuse
void handle_updated_modules();
void prune_modules();
void load_sepolicy_rule();
void load_system_prop();
void disable_all_modules();
void exec_common_scripts(const std::string &dir, bool wait);
void exec_stage_script(const std::string &stage, bool block);

// Shared building blocks
std::vector<std::pair<std::string, std::string>> get_common_script_envs(const OptStr &module_id);
std::map<std::string, std::string> read_module_prop(const std::filesystem::path &module_path);
void foreach_module(ModuleType type,
                    const std::function<void(const std::filesystem::path &)> &f);
void exec_script(const std::filesystem::path &path, bool wait);

} // namespace apd::module
