#pragma once
// Root shell (su/kp multicall entry). Mirrors ../apd/src/apd.rs.
#include <string>
#include <vector>

namespace apd {
// The kernel hands us root and execs us as su/kp. Parses su-style options and
// exec()s the target shell. Returns a process exit code (normally exec replaces us).
int root_shell(const std::vector<std::string> &args);
} // namespace apd
