#pragma once
// magiskpolicy / SELinux policy patching. Mirrors ../apd/src/sepolicy.rs.
// (Heaviest subsystem: backed by vendored Magisk magiskpolicy + libsepol in phase 4.)
#include <string>
#include <vector>

namespace apd::sepolicy {
// argv[0]=="magiskpolicy" multicall entry. Returns a process exit code.
int policy_main(const std::vector<std::string> &args);
// `apd sepolicy ...` subcommand (args after the subcommand name).
void execute_args(const std::vector<std::string> &args);
// Live-apply a module's sepolicy.rule file (magiskpolicy --live --apply FILE).
void apply_rule_file_live(const std::string &rule_file);
// Load live policy, apply built-in Magisk rules, write to /sys/fs/selinux/load.
void apply_magisk_rules_live();
} // namespace apd::sepolicy
