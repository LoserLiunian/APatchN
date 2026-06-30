#pragma once
// Process spawning helper. Covers the std::process::Command patterns in
// module.rs / event.rs (env overrides, cwd, process-group, cgroup switch).
#include <optional>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace apd::proc {

struct Cmd {
    std::string program;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env; // added/overridden in child
    std::optional<std::string> cwd;
    bool new_process_group = false;   // setpgid(0,0) in child
    bool child_switch_cgroups = false; // utils::switch_cgroups() in child
    int stdout_fd = -1;               // dup2 onto stdout if >= 0
};

// fork+exec. Returns child pid. Throws on fork failure.
pid_t spawn(const Cmd &cmd);

// waitpid; returns exit code, or -signal if killed, or -1 on error.
int wait_pid(pid_t pid);

// spawn + wait; true iff the child exited 0.
bool run(const Cmd &cmd);

} // namespace apd::proc
