#pragma once
#include <string>
#include <vector>

namespace apd::cli {
// Entry point. `args` includes argv[0]. Returns a process exit code.
int run(const std::vector<std::string> &args);
} // namespace apd::cli
