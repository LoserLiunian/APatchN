#pragma once
// Tiny JSON writer for module listing. Replaces serde_json for the only use
// site: pretty-printing a list of string->string maps (module.rs list_modules).
#include <map>
#include <string>
#include <vector>

namespace apd::json {

std::string escape(const std::string &s);

// serde_json::to_string_pretty-compatible (2-space indent) for an array of objects.
std::string to_pretty(const std::vector<std::map<std::string, std::string>> &arr);

} // namespace apd::json
