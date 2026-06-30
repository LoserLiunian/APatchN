#include "process.hpp"
#include "result.hpp"
#include "utils.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace apd::proc {

pid_t spawn(const Cmd &cmd) {
    pid_t pid = ::fork();
    if (pid < 0) bailf("fork failed: {}", std::strerror(errno));
    if (pid == 0) {
        // --- child ---
        if (cmd.new_process_group) ::setpgid(0, 0);
        if (cmd.child_switch_cgroups) utils::switch_cgroups();
        // A failed chdir must not silently run the program in the wrong dir
        // (std::process::Command reports this as a spawn error); abort the child.
        if (cmd.cwd && ::chdir(cmd.cwd->c_str()) != 0) ::_exit(127);
        if (cmd.stdout_fd >= 0) ::dup2(cmd.stdout_fd, STDOUT_FILENO);
        for (const auto &[k, v] : cmd.env) ::setenv(k.c_str(), v.c_str(), 1);

        std::vector<char *> argv;
        argv.reserve(cmd.args.size() + 2);
        argv.push_back(const_cast<char *>(cmd.program.c_str()));
        for (const auto &a : cmd.args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);

        ::execvp(cmd.program.c_str(), argv.data());
        ::_exit(127); // exec failed
    }
    return pid;
}

int wait_pid(pid_t pid) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return -1;
}

bool run(const Cmd &cmd) { return wait_pid(spawn(cmd)) == 0; }

} // namespace apd::proc
