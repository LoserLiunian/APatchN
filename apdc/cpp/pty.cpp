#include "pty.hpp"

#include "defs.hpp"
#include "logging.hpp"
#include "result.hpp"
#include "utils.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <pthread.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

#ifndef TIOCGPTN
#define TIOCGPTN _IOR('T', 0x30, unsigned int)
#endif

namespace fs = std::filesystem;

namespace apd::pty {
namespace {

struct termios g_old_stdin;
bool g_have_old = false;

unsigned int get_pty_num(int fd) {
    unsigned int n = 0;
    if (::ioctl(fd, TIOCGPTN, &n) != 0) bail("TIOCGPTN failed");
    return n;
}

void watch_sigwinch_async(int slave) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGWINCH);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    std::thread([slave]() {
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGWINCH);
        pthread_sigmask(SIG_UNBLOCK, &s, nullptr);
        int sig = 0;
        for (;;) {
            struct winsize w;
            if (::ioctl(1, TIOCGWINSZ, &w) < 0) continue;
            ::ioctl(slave, TIOCSWINSZ, &w);
            if (sigwait(&s, &sig) != 0) break;
        }
    }).detach();
}

void set_stdin_raw() {
    struct termios t;
    if (tcgetattr(0, &t) != 0) return;
    g_old_stdin = t;
    g_have_old = true;
    cfmakeraw(&t);
    if (tcsetattr(0, TCSAFLUSH, &t) != 0) tcsetattr(0, TCSADRAIN, &t);
}

void restore_stdin() {
    if (g_have_old) {
        tcsetattr(0, TCSAFLUSH, &g_old_stdin);
        g_have_old = false;
    }
}

void pump(int from, int to) {
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(from, buf, sizeof buf);
        if (n <= 0) return;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = ::write(to, buf + off, static_cast<std::size_t>(n - off));
            if (w <= 0) return;
            off += w;
        }
    }
}

void pump_stdin_async(int ptmx) {
    set_stdin_raw();
    std::thread([ptmx]() { pump(0, ptmx); }).detach();
}

void pump_stdout_blocking(int ptmx) {
    pump(ptmx, 1);
    restore_stdin();
}

void create_transfer(int ptmx_fd) {
    pid_t pid = ::fork();
    if (pid < 0) bail("fork");
    if (pid == 0) {
        ::close(ptmx_fd); // child doesn't need the master fd (matches Rust OwnedFd drop)
        return;           // child continues to exec the shell
    }

    int ptmx_w = ::dup(ptmx_fd);
    watch_sigwinch_async(ptmx_w);
    pump_stdin_async(ptmx_fd);
    pump_stdout_blocking(ptmx_w);

    int status = -1;
    while (::waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    ::_exit(status);
}

} // namespace

void prepare_pty() {
    bool tty_in = isatty(0), tty_out = isatty(1), tty_err = isatty(2);
    if (!tty_in && !tty_out && !tty_err) return;

    std::string pts_path = std::string(utils::get_tmp_path()) + "/" + defs::PTS_NAME;
    std::error_code ec;
    if (!fs::exists(pts_path, ec)) pts_path = "/dev/pts";

    std::string ptmx_path = pts_path + "/ptmx";
    int ptmx_fd = ::open(ptmx_path.c_str(), O_RDWR);
    if (ptmx_fd < 0) bailf("open {} failed", ptmx_path);
    if (grantpt(ptmx_fd) != 0) bail("grantpt failed");
    if (unlockpt(ptmx_fd) != 0) bail("unlockpt failed");

    unsigned int num = get_pty_num(ptmx_fd);
    create_transfer(ptmx_fd); // forks; only the child returns here
    setsid();

    std::string pty_path = pts_path + "/" + std::to_string(num);
    int pty_fd = ::open(pty_path.c_str(), O_RDWR);
    if (pty_fd < 0) bailf("open {} failed", pty_path);
    if (tty_in) ::dup2(pty_fd, 0);
    if (tty_out) ::dup2(pty_fd, 1);
    if (tty_err) ::dup2(pty_fd, 2);
}

} // namespace apd::pty
