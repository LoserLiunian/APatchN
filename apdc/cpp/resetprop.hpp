#pragma once
// resetprop — Magisk-compatible property tool. Mirrors ../apd/src/resetprop.rs.
// (Heavy subsystem: backed by vendored Magisk resetprop in phase 4.)
#include <string>
#include <vector>

namespace apd::resetprop {
// argv[0]=="resetprop" multicall entry. Returns a process exit code.
int resetprop_main(const std::vector<std::string> &args);
// `apd resetprop ...` subcommand (args after the subcommand name).
void execute_args(const std::vector<std::string> &args);
// Equivalent to `resetprop -n --file <path>`; used by module::load_system_prop.
void load_system_prop_file(const std::string &path);
} // namespace apd::resetprop
