#include "sepolicy.hpp"

#include "defs.hpp"
#include "logging.hpp"
#include "result.hpp"

#include <magiskpolicy.hpp>

#include <cstdio>
#include <memory>

// Defined in the vendored statement.cpp (magiskpolicy lib).
void statement_help();

namespace apd::sepolicy {
namespace {

constexpr const char *LIVE_POLICY = "/sys/fs/selinux/policy";
constexpr const char *LIVE_LOAD = "/sys/fs/selinux/load";

struct SepolArgs {
    OptStr load;
    bool load_split = false;
    bool compile_split = false;
    OptStr save;
    bool live = false;
    bool magisk = false;
    bool print_rules = false;
    bool help = false;
    std::vector<std::string> apply;
    std::vector<std::string> policies;
};

struct SepolParseResult {
    SepolArgs args;
    bool want_version = false;
};

SepolParseResult parse(const std::vector<std::string> &a, std::size_t start) {
    SepolParseResult r;
    auto &args = r.args;
    for (std::size_t i = start; i < a.size(); ++i) {
        std::string t = a[i];
        // Accept clap-style --flag=VALUE long-option form.
        std::string attached;
        bool has_attached = false;
        if (t.size() > 2 && t.compare(0, 2, "--") == 0) {
            auto eq = t.find('=');
            if (eq != std::string::npos) {
                attached = t.substr(eq + 1);
                t = t.substr(0, eq);
                has_attached = true;
            }
        }
        auto take = [&]() -> std::string {
            if (has_attached) return attached;
            ensuref(i + 1 < a.size(), "{} requires an argument", t);
            return a[++i];
        };
        if (t == "--load") args.load = take();
        else if (t == "--load-split") args.load_split = true;
        else if (t == "--compile-split") args.compile_split = true;
        else if (t == "--save") args.save = take();
        else if (t == "--live") args.live = true;
        else if (t == "--magisk") args.magisk = true;
        else if (t == "--apply") args.apply.push_back(take());
        else if (t == "--print-rules") args.print_rules = true;
        else if (t == "--version") r.want_version = true;
        else if (t == "-h" || t == "--help") args.help = true;
        else args.policies.push_back(a[i]); // preserve original (with attached '=') for unknown
    }
    return r;
}

// Magiskpolicy's parse_statement() handles ONE statement per call. Rust's
// SePolicy::load_rules() splits on newlines so a multi-line string from
// --apply / module sepolicy.rule works as expected; mirror that here.
void load_rules_multiline(::sepolicy *sepol, const std::string &block) {
    std::size_t pos = 0;
    while (pos <= block.size()) {
        auto nl = block.find('\n', pos);
        std::string line = (nl == std::string::npos) ? block.substr(pos) : block.substr(pos, nl - pos);
        // Trim trailing CR (CRLF inputs).
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Skip empty/whitespace-only lines.
        auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos) sepol->parse_statement(line.c_str());
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

using SepolPtr = std::unique_ptr<::sepolicy>;

SepolPtr load_policy(const SepolArgs &cli) {
    int load_count = (cli.load ? 1 : 0) + (cli.compile_split ? 1 : 0) + (cli.load_split ? 1 : 0);
    ensure(load_count <= 1, "Multiple load source supplied");

    ::sepolicy *p;
    if (cli.load) p = ::sepolicy::from_file(cli.load->c_str());
    else if (cli.load_split) p = ::sepolicy::from_split();
    else if (cli.compile_split) p = ::sepolicy::compile_split();
    else p = ::sepolicy::from_file(LIVE_POLICY);

    ensure(p != nullptr, "Cannot load policy");
    return SepolPtr(p);
}

void execute_next(const SepolArgs &cli, ::sepolicy *sepol) {
    if (cli.print_rules) {
        ensure(!cli.magisk && cli.apply.empty() && cli.policies.empty() && !cli.live && !cli.save,
               "Cannot print rules with other options");
        // ponytail: the vendored Magisk v23 sepolicy lib has no rule iterator;
        // porting one is large work. Surface the limitation instead of silently
        // doing the wrong thing.
        bail("--print-rules is not supported (vendored libsepol lacks an iterator)");
    }

    if (cli.magisk) sepol->magisk_rules();
    for (const auto &file : cli.apply) sepol->load_rule_file(file.c_str());
    for (const auto &stmt : cli.policies) load_rules_multiline(sepol, stmt);

    if (cli.live)
        ensure(sepol->to_file(LIVE_LOAD), "Cannot apply policy");
    if (cli.save)
        ensure(sepol->to_file(cli.save->c_str()), "Cannot dump policy");
}

void print_usage(const char *cmd) {
    std::fprintf(stderr,
        "MagiskPolicy - SELinux Policy Patch Tool\n\n"
        "Usage: %s [--options...] [policy statements...]\n\n"
        "Options:\n"
        "   --help            show help\n"
        "   --load FILE       load monolithic sepolicy from FILE\n"
        "   --load-split      load from precompiled sepolicy\n"
        "   --compile-split   compile split cil policies (unsupported in apd)\n"
        "   --save FILE       dump monolithic sepolicy to FILE\n"
        "   --live            immediately load sepolicy into the kernel\n"
        "   --magisk          apply built-in Magisk sepolicy rules\n"
        "   --apply FILE      apply rules from FILE (may repeat)\n\n"
        "If no --load/--load-split/--compile-split is given, the live policy\n"
        "(%s) is used.\n\n",
        cmd, LIVE_POLICY);
    statement_help();
}

} // namespace

void execute_args(const std::vector<std::string> &args) {
    auto pr = parse(args, 0);
    if (pr.want_version) {
        std::printf("magiskpolicy %s\n", apd::defs::VERSION_NAME);
        return;
    }
    if (pr.args.help) {
        print_usage("magiskpolicy");
        return;
    }
    auto sepol = load_policy(pr.args);
    execute_next(pr.args, sepol.get());
}

int policy_main(const std::vector<std::string> &args) {
    auto pr = parse(args, 1);
    if (pr.want_version) {
        std::printf("magiskpolicy %s\n", apd::defs::VERSION_NAME);
        return 0;
    }
    if (pr.args.help) {
        print_usage(args.empty() ? "magiskpolicy" : args[0].c_str());
        return 0;
    }
    try {
        auto sepol = load_policy(pr.args);
        execute_next(pr.args, sepol.get());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "magiskpolicy: %s\n", e.what());
        return 1;
    }
    return 0;
}

void apply_rule_file_live(const std::string &rule_file) {
    auto sepol = SepolPtr(::sepolicy::from_file(LIVE_POLICY));
    ensure(sepol != nullptr, "Cannot load live policy");
    sepol->load_rule_file(rule_file.c_str());
    ensure(sepol->to_file(LIVE_LOAD), "Cannot apply policy");
}

void apply_magisk_rules_live() {
    auto sepol = SepolPtr(::sepolicy::from_file(LIVE_POLICY));
    ensure(sepol != nullptr, "Cannot load live policy");
    sepol->magisk_rules();
    ensure(sepol->to_file(LIVE_LOAD), "Cannot apply policy");
}

} // namespace apd::sepolicy
