#include "cli.hpp"

#include "apd.hpp"
#include "defs.hpp"
#include "event.hpp"
#include "logging.hpp"
#include "lua.hpp"
#include "module.hpp"
#include "module_config.hpp"
#include "resetprop.hpp"
#include "result.hpp"
#include "sepolicy.hpp"
#include "supercall.hpp"
#include "utils.hpp"

#include <cstdio>
#include <cstdlib>

namespace apd::cli {
namespace {

// `apd module config [--internal NAME] <get|set|list|delete|clear> ...`
// Mirrors cli.rs ModuleConfigCmd. `a[0]` == "config".
int config_dispatch(const std::vector<std::string> &a) {
    OptStr internal;
    bool temp = false, use_stdin = false;
    std::vector<std::string> pos;
    constexpr std::string_view kInternal = "--internal=";
    for (std::size_t i = 1; i < a.size(); ++i) {
        const std::string &t = a[i];
        if (t == "--internal") {
            ensure(i + 1 < a.size(), "--internal requires a value");
            internal = a[++i];
        } else if (t.rfind(kInternal, 0) == 0) {
            internal = t.substr(kInternal.size());
        } else if (t == "--temp" || t == "-t") {
            temp = true;
        } else if (t == "--stdin") {
            use_stdin = true;
        } else if (!t.empty() && t[0] == '-') {
            bailf("unexpected option: {}", t);
        } else {
            pos.push_back(t);
        }
    }
    ensure(!pos.empty(), "module config: missing subcommand");
    const std::string &sub = pos[0];

    std::string module_id;
    if (internal) {
        module_id = "internal." + *internal;
    } else {
        const char *e = std::getenv("AP_MODULE");
        ensure(e != nullptr && e[0] != '\0',
               "This command must be run in the context of a module or passed --internal <name>");
        module_id = e;
    }

    using CT = module_config::ConfigType;
    CT ct = temp ? CT::Temp : CT::Persist;

    if (sub == "get") {
        ensure(pos.size() > 1, "config get: missing key");
        auto cfg = module_config::merge_configs(module_id);
        auto it = cfg.find(pos[1]);
        if (it == cfg.end()) bailf("Key '{}' not found", pos[1]);
        std::printf("%s\n", it->second.c_str());
    } else if (sub == "set") {
        ensure(pos.size() > 1, "config set: missing key");
        module_config::validate_config_key(pos[1]);
        std::string value;
        if (pos.size() > 2 && !use_stdin) {
            value = pos[2];
        } else {
            char buf[4096];
            std::size_t r;
            while ((r = std::fread(buf, 1, sizeof buf, stdin)) > 0) value.append(buf, r);
        }
        module_config::validate_config_value(value);
        module_config::set_config_value(module_id, pos[1], value, ct);
    } else if (sub == "list") {
        auto cfg = module_config::merge_configs(module_id);
        if (cfg.empty()) {
            std::printf("No config entries found\n");
        } else {
            for (const auto &[k, v] : cfg) std::printf("%s=%s\n", k.c_str(), v.c_str());
        }
    } else if (sub == "delete") {
        ensure(pos.size() > 1, "config delete: missing key");
        module_config::delete_config_value(module_id, pos[1], ct);
    } else if (sub == "clear") {
        module_config::clear_config(module_id, ct);
    } else {
        bailf("unknown config subcommand: {}", sub);
    }
    return 0;
}

// `apd [-s KEY] module <sub> [args]` — mirrors cli.rs Module subcommands.
int module_dispatch(const std::vector<std::string> &a) {
    ensure(!a.empty(), "module: missing subcommand");
    const std::string &sub = a[0];

    auto arg = [&](std::size_t n) -> const std::string & {
        ensuref(a.size() > n, "module {}: missing argument", sub);
        return a[n];
    };

    if (sub == "install") {
        module::install_module(arg(1));
    } else if (sub == "uninstall") {
        module::uninstall_module(arg(1));
    } else if (sub == "undo-uninstall") {
        module::undo_uninstall_module(arg(1));
    } else if (sub == "enable") {
        module::enable_module(arg(1));
    } else if (sub == "disable") {
        module::disable_module(arg(1));
    } else if (sub == "action") {
        module::run_action(arg(1));
    } else if (sub == "lua") {
        lua::run_lua(arg(1), arg(2), false, true);
    } else if (sub == "list") {
        module::list_modules();
    } else if (sub == "config") {
        return config_dispatch(a);
    } else {
        bailf("unknown module subcommand: {}", sub);
    }
    return 0;
}

} // namespace

int run(const std::vector<std::string> &args) {
    log::init();

    // the kernel execs su with argv[0] = ".../kp" / ".../su" / "su" / "kp"
    const std::string arg0 = args.empty() ? std::string() : args[0];
    if (arg0.ends_with("kp") || arg0.ends_with("su")) return apd::root_shell(args);
    if (arg0.ends_with("resetprop")) return resetprop::resetprop_main(args);
    if (arg0.ends_with("magiskpolicy")) return sepolicy::policy_main(args);

    // ---- top-level parse: optional -s/--superkey, then a subcommand ----
    constexpr std::string_view kLong = "--superkey=";
    OptStr superkey;
    std::size_t i = 1;
    std::string cmd;
    for (; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "-s" || a == "--superkey") {
            ensure(i + 1 < args.size(), "missing value for --superkey");
            superkey = args[++i];
        } else if (a.rfind(kLong, 0) == 0) {
            superkey = a.substr(kLong.size());
        } else if (a.rfind("-s", 0) == 0 && a.size() > 2) {
            superkey = a.substr(2); // -sKEY
        } else if (a == "--version" || a == "-V") {
            std::printf("apd %s\n", apd::defs::VERSION_NAME);
            return 0;
        } else if (a == "--help" || a == "-h") {
            std::fprintf(stderr,
                "apd %s — APatch userspace daemon\n\n"
                "Usage: apd [-s|--superkey KEY] <subcommand> [args...]\n\n"
                "Subcommands:\n"
                "  post-fs-data | services | boot-completed | uid-listener\n"
                "  module install|uninstall|undo-uninstall|enable|disable|action|lua|list|config\n"
                "  resetprop ... | sepolicy ...\n",
                apd::defs::VERSION_NAME);
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            bailf("unexpected option: {}", a);
        } else {
            cmd = a;
            ++i;
            break;
        }
    }
    std::vector<std::string> rest(args.begin() + static_cast<long>(i), args.end());

    ensure(!cmd.empty(), "no subcommand given");
    log::info("command: {}", cmd);

    if (superkey) supercall::privilege_apd_profile(superkey);

    if (cmd == "post-fs-data") {
        event::on_post_data_fs(superkey);
    } else if (cmd == "services") {
        event::on_services(superkey);
    } else if (cmd == "boot-completed") {
        event::on_boot_completed(superkey);
    } else if (cmd == "uid-listener") {
        event::start_uid_listener();
    } else if (cmd == "resetprop") {
        resetprop::execute_args(rest);
    } else if (cmd == "sepolicy") {
        sepolicy::execute_args(rest);
    } else if (cmd == "module") {
        utils::switch_mnt_ns(1);
        return module_dispatch(rest);
    } else {
        bailf("unknown subcommand: {}", cmd);
    }
    return 0;
}

} // namespace apd::cli
