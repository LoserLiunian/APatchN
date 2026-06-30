#pragma once
// Zip extraction via vendored miniz. Replaces the `zip` + `zip-extensions`
// crates for module install. miniz supports store/deflate (the vast majority
// of module zips); LZMA/XZ entries are not supported.
// ponytail: deflate/store only — add an lzma path if a real module needs it.
#include <string>
#include <vector>

namespace apd::zip {

// Extract a single entry to memory. Throws if the entry is missing.
std::string extract_file_to_memory(const std::string &zip_path, const std::string &entry);

// All entry names (files and directories).
std::vector<std::string> list_names(const std::string &zip_path);

// Extract the whole archive into dest_dir, preserving directory structure and
// unix permissions. Rejects entries that would escape dest_dir (zip-slip).
void extract_all(const std::string &zip_path, const std::string &dest_dir);

} // namespace apd::zip
