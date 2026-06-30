#include "apd.hpp"

#include "defs.hpp"
#include "logging.hpp"
#include "pty.hpp"
#include "result.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace apd {
namespace {

void print_usage() {
    std::printf(
        "APatch\n\nUsage: <command> [options] [-] [user [argument...]]\n"
        "  -c, --command COMMAND        pass COMMAND to the invoked shell\n"
        "  -h, --help                   display this help message and exit\n"
        "  -l, --login                  pretend the shell to be a login shell\n"
        "  -p, --preserve-environment   preserve the entire environment\n"
        "  -s, --shell SHELL            use SHELL instead of the default /system/bin/sh\n"
        "  -v, --version                display version number and exit\n"
        "  -V                           display version code and exit\n"
        "  -M, --mount-master           force run in the global mount namespace\n"
        "      --no-pty                 Do not allocate a new pseudo terminal.\n");
}

void set_identity(uint32_t uid, uint32_t gid) {
    ::setresgid(gid, gid, gid);
    ::setresuid(uid, uid, uid);
}

bool parse_u32(const std::string &s, uint32_t &out) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    out = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
    return true;
}

std::string trimmed(const std::string &s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

} // namespace

int root_shell(const std::vector<std::string> &env_args) {
    // we are root now; the kernel set this.
    // collapse everything after the first "-c" into a single command argument
    std::vector<std::string> args;
    {
        auto it = std::find(env_args.begin(), env_args.end(), "-c");
        if (it != env_args.end()) {
            args.assign(env_args.begin(), it);
            args.push_back("-c");
            std::string rest;
            for (auto j = it + 1; j != env_args.end(); ++j) {
                if (!rest.empty()) rest += " ";
                rest += *j;
            }
            // push the command whenever -c had at least one token after it, even
            // if it collapses to "" (matches Rust: `su -c ""` runs an empty command).
            if (it + 1 != env_args.end()) args.push_back(rest);
        } else {
            args = env_args;
        }
    }
    // support getopt_long quirks: -mm -> -M, -cn -> -z
    for (auto &e : args) {
        if (e == "-mm") e = "-M";
        else if (e == "-cn") e = "-z";
    }

    bool opt_h = false, opt_l = false, opt_p = false, opt_v = false, opt_V = false, opt_M = false,
         no_pty = false;
    OptStr command, shell_opt;
    std::vector<std::string> freeargs;
    bool end_opts = false;
    // getopts-equivalent: "--" terminator, "--long=value", attached short values
    // ("-sVALUE"), and bundled short flags ("-lp").
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string &a = args[i];
        auto take_value = [&](const std::string &attached) -> std::string {
            if (!attached.empty()) return attached;
            if (i + 1 >= args.size()) {
                print_usage();
                std::exit(255); // getopts: missing arg -> usage + exit(-1)
            }
            return args[++i];
        };

        if (!end_opts && a == "--") {
            end_opts = true;
            continue;
        }
        if (end_opts || a.empty() || a[0] != '-' || a == "-") {
            freeargs.push_back(a);
            continue;
        }

        if (a.rfind("--", 0) == 0) { // long option, possibly --opt=value
            std::string name = a, val;
            bool has_val = false;
            if (auto eq = a.find('='); eq != std::string::npos) {
                name = a.substr(0, eq);
                val = a.substr(eq + 1);
                has_val = true;
            }
            if (name == "--no-pty") no_pty = true;
            else if (name == "--help") opt_h = true;
            else if (name == "--login") opt_l = true;
            else if (name == "--preserve-environment") opt_p = true;
            else if (name == "--mount-master") opt_M = true;
            else if (name == "--version") opt_v = true;
            else if (name == "--command") command = has_val ? val : take_value("");
            else if (name == "--shell") shell_opt = has_val ? val : take_value("");
            else {
                std::printf("Unrecognized option '%s'\n", a.c_str());
                print_usage();
                return -1;
            }
            continue;
        }

        bool bad = false; // short cluster
        for (std::size_t k = 1; k < a.size(); ++k) {
            char c = a[k];
            if (c == 'h') opt_h = true;
            else if (c == 'l') opt_l = true;
            else if (c == 'p') opt_p = true;
            else if (c == 'M') opt_M = true;
            else if (c == 'v') opt_v = true;
            else if (c == 'V') opt_V = true;
            else if (c == 'c') { command = take_value(a.substr(k + 1)); break; }
            else if (c == 's') { shell_opt = take_value(a.substr(k + 1)); break; }
            else { bad = true; break; }
        }
        if (bad) {
            std::printf("Unrecognized option '%s'\n", a.c_str());
            print_usage();
            return -1;
        }
    }

    if (opt_h) {
        print_usage();
        return 0;
    }
    if (opt_v) {
        std::printf("%s:APatch\n", defs::VERSION_NAME);
        return 0;
    }
    if (opt_V) {
        std::printf("%s\n", defs::VERSION_CODE);
        return 0;
    }

    std::string shell = shell_opt ? *shell_opt : "/system/bin/sh";
    bool is_login = opt_l, preserve_env = opt_p, mount_master = opt_M;

    std::vector<std::string> shell_args;
    if (command) shell_args = {"-c", *command};

    uint32_t uid = ::getuid(), gid = ::getgid();
    std::size_t idx = 0;
    if (!freeargs.empty() && freeargs[0] == "-") {
        is_login = true;
        idx = 1;
    }
    if (idx < freeargs.size()) {
        const std::string &name = freeargs[idx];
        if (struct passwd *pw = ::getpwnam(name.c_str())) {
            uid = pw->pw_uid;
        } else if (uint32_t parsed; parse_u32(name, parsed)) {
            uid = parsed;
        } else {
            uid = 0;
        }
    }

    const char *arg0 = is_login ? "-" : shell.c_str();

    if (!preserve_env) {
        if (struct passwd *pw = ::getpwuid(uid)) {
            ::setenv("HOME", pw->pw_dir, 1);
            ::setenv("USER", pw->pw_name, 1);
            ::setenv("LOGNAME", pw->pw_name, 1);
            ::setenv("SHELL", shell.c_str(), 1);
        }
    }

    // append /data/adb/ap/bin to PATH
    {
        std::string bin = defs::BINARY_DIR;
        while (!bin.empty() && bin.back() == '/') bin.pop_back();
        const char *cur = ::getenv("PATH");
        std::string np = cur ? std::string(cur) : std::string();
        if (!np.empty()) np += ":";
        np += bin;
        ::setenv("PATH", np.c_str(), 1);
    }

    {
        std::error_code ec;
        if (fs::exists(defs::AP_RC_PATH, ec) && ::getenv("ENV") == nullptr)
            ::setenv("ENV", defs::AP_RC_PATH, 1);
    }

    if (!no_pty) {
        try {
            pty::prepare_pty();
        } catch (const std::exception &e) {
            log::error("failed to prepare pty: {}", e.what());
        }
    }

    // pre-exec steps (we exec in-process, like Command::exec)
    utils::umask(022);
    utils::switch_cgroups();

    std::string gns = "0";
    {
        std::ifstream in(defs::GLOBAL_NAMESPACE_FILE);
        if (in) {
            std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            gns = s;
        }
    }
    if (trimmed(gns) == "1" || mount_master) {
        try {
            utils::switch_mnt_ns(1);
        } catch (const std::exception &) {
        }
    }

    set_identity(uid, gid);

    std::vector<char *> argv;
    std::string arg0s = arg0;
    argv.push_back(const_cast<char *>(arg0s.c_str()));
    for (auto &a : shell_args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);

    ::execvp(shell.c_str(), argv.data());
    bailf("exec {} failed: {}", shell, std::strerror(errno));
}

} // namespace apd
