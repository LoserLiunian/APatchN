#pragma once
// Misc helpers. Mirrors ../apd/src/utils.rs (process-spawn helpers are added
// in the event phase, where their exact call shapes are known).
#include <optional>
#include <string>
#include "defs.hpp"

namespace apd::utils {

// Create the file if it does not exist; ok if it already exists as a regular
// file. Throws otherwise.
void ensure_file_exists(const std::string &path);

// mkdir -p; throws if the path exists as a non-directory.
void ensure_dir_exists(const std::string &path);

// chmod 0755.
void ensure_binary(const std::string &path);

// Android system property read. nullopt if unset/empty.
OptStr getprop(const std::string &prop);

bool is_safe_mode(const OptStr &superkey);

// setns(CLONE_NEWNS) into /proc/<pid>/ns/mnt, preserving cwd. Throws on failure.
void switch_mnt_ns(int pid);

void switch_cgroups();

void umask(unsigned mask);

bool has_magisk();

// "/sbin" (legacy) or "/debug_ramdisk", else "".
std::string get_tmp_path();

} // namespace apd::utils
