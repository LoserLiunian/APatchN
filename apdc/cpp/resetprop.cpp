#include "resetprop.hpp"

#include "defs.hpp"
#include "logging.hpp"
#include "result.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <map>
#include <string>
#include <sys/system_properties.h>
#include <unistd.h>
#include <vector>

// Implemented in the vendored Magisk resetprop (magiskresetprop lib).
int setprop(const char *name, const char *value, bool prop_svc);
std::string getprop(const char *name, bool persist);
void getprops(void (*callback)(const char *, const char *, void *), void *cookie, bool persist);
int delprop(const char *name, bool persist);
void load_prop_file(const char *filename, bool prop_svc);
bool persist_setprop(const char *name, const char *value);

namespace apd::resetprop {
namespace {

void usage() {
    std::fprintf(stderr,
        "resetprop - Magisk-compatible system property tool\n\n"
        "Usage: resetprop [flags] [NAME] [VALUE]\n"
        "  (no args)        list all properties\n"
        "  NAME             get property\n"
        "  NAME VALUE       set property\n"
        "  -d, --delete NAME   delete property\n"
        "  -f, --file FILE     load properties from FILE\n"
        "  -w, --wait NAME [VALUE]  wait for property\n"
        "      --timeout SEC   timeout for --wait\n"
        "  -n, --skip-svc      bypass property_service (direct write)\n"
        "  -p, --persistent    also operate on persistent storage\n"
        "  -P                  persistent-only read\n"
        "  -v, --verbose       verbose\n"
        "  -Z                  show SELinux context (unsupported)\n"
        "  -c, --compact       compact prop area (unsupported)\n");
}

double now_sec() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Magisk semantics, mirrored from ksu_props sys_prop::wait():
//   value == nullptr : wait until the property EXISTS.
//   value != nullptr : wait until the property's current value DIFFERS from *value.
// ponytail: polling (50ms) instead of __system_property_wait; fine for a CLI.
bool do_wait(const std::string &name, const std::string *value, const OptStr &timeout) {
    double deadline = -1;
    if (timeout) deadline = now_sec() + std::strtod(timeout->c_str(), nullptr);
    for (;;) {
        if (value == nullptr) {
            // Phase: wait for existence.
            if (__system_property_find(name.c_str()) != nullptr) return true;
        } else {
            std::string cur = getprop(name.c_str(), false);
            if (cur != *value) return true;
        }
        if (deadline >= 0 && now_sec() >= deadline) return false;
        usleep(50 * 1000);
    }
}

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// Look up the SELinux context label of a property via bionic's hidden API.
// __system_property_get_context is present on Android 7+ but not in NDK headers,
// so resolve it lazily; returns empty when unavailable.
// ponytail: dlsym instead of vendoring the property_info_parser ourselves.
std::string get_prop_context(const char *name) {
    using fn_t = const char *(*)(const char *);
    static fn_t fn = reinterpret_cast<fn_t>(::dlsym(RTLD_DEFAULT, "__system_property_get_context"));
    if (!fn) return {};
    const char *ctx = fn(name);
    return ctx ? std::string(ctx) : std::string();
}

// Parse + execute. Returns process exit code (2 == wait timeout).
int run(const std::vector<std::string> &args, std::size_t start) {
    bool skip_svc = false, persistent = false, persist_only = false, del = false, verbose = false,
         wait = false, compact = false, show_context = false, want_version = false;
    OptStr timeout, file;
    std::vector<std::string> pos;

    auto next = [&](std::size_t &i) -> std::string {
        if (i + 1 >= args.size()) {
            usage();
            std::exit(1);
        }
        return args[++i];
    };

    for (std::size_t i = start; i < args.size(); ++i) {
        const std::string &t = args[i];
        if (t == "-f" || t == "--file") file = next(i);
        else if (t.rfind("--file=", 0) == 0) file = t.substr(7);
        else if (t.rfind("-f", 0) == 0 && t.rfind("--", 0) != 0 && t.size() > 2) file = t.substr(2);
        else if (t == "--timeout") timeout = next(i);
        else if (t.rfind("--timeout=", 0) == 0) timeout = t.substr(10);
        else if (t.rfind("--", 0) == 0) {
            if (t == "--skip-svc") skip_svc = true;
            else if (t == "--persistent") persistent = true;
            else if (t == "--delete") del = true;
            else if (t == "--verbose") verbose = true;
            else if (t == "--wait") wait = true;
            else if (t == "--compact") compact = true;
            else if (t == "--version") want_version = true;
            else if (t == "--help") { usage(); return 0; }
            else { usage(); return 1; }
        } else if (t.size() > 1 && t[0] == '-') {
            for (std::size_t k = 1; k < t.size(); ++k) {
                switch (t[k]) {
                case 'n': skip_svc = true; break;
                case 'p': persistent = true; break;
                case 'P': persist_only = true; break;
                case 'd': del = true; break;
                case 'v': verbose = true; break;
                case 'w': wait = true; break;
                case 'c': compact = true; break;
                case 'Z': show_context = true; break;
                case 'h': usage(); return 0;
                default: usage(); return 1;
                }
            }
        } else {
            pos.push_back(t);
        }
    }
    if (want_version) {
        std::printf("resetprop %s\n", apd::defs::VERSION_NAME);
        return 0;
    }
    (void)verbose; // -v silently consumed; APatchD logcat is already DEBUG-level.

    int special = int(wait) + int(del) + int(compact) + int(file.has_value());
    if (special > 1) {
        std::fprintf(stderr, "resetprop: multiple operation modes detected\n");
        return 1;
    }

    if (wait) {
        if (pos.empty()) {
            std::fprintf(stderr, "resetprop: --wait requires a property name\n");
            return 1;
        }
        const std::string *value = pos.size() >= 2 ? &pos[1] : nullptr;
        return do_wait(pos[0], value, timeout) ? 0 : 2;
    }

    if (compact) {
        // The vendored Magisk systemproperties lib does not expose a compaction
        // entry point. ponytail: not supported; exit 1 like Rust on "nothing to compact".
        std::fprintf(stderr, "resetprop: compact unsupported by vendored prop area\n");
        return 1;
    }

    if (file) {
        load_prop_file(file->c_str(), !skip_svc);
        return 0;
    }

    if (del) {
        if (pos.empty()) {
            std::fprintf(stderr, "resetprop: --delete requires a property name\n");
            return 1;
        }
        // Mirror Rust: only -p (persistent) triggers /data/property deletion; -P alone
        // only deletes the runtime entry.
        int r = delprop(pos[0].c_str(), persistent);
        if (r != 0) {
            std::fprintf(stderr, "resetprop: %s not found\n", pos[0].c_str());
            return 1;
        }
        return 0;
    }

    if (pos.empty()) {
        // Rust merges runtime + persistent with PERSISTENT values OVERRIDDEN by runtime
        // (skip persist if name already present from sys), then sorts and (optionally)
        // replaces values with the SELinux context.
        std::map<std::string, std::string> merged;
        if (!persist_only) {
            getprops([](const char *n, const char *v, void *c) {
                auto &m = *static_cast<std::map<std::string, std::string> *>(c);
                m.emplace(n, v);
            }, &merged, false);
        }
        if (persistent || persist_only) {
            std::map<std::string, std::string> persist_props;
            getprops([](const char *n, const char *v, void *c) {
                auto &m = *static_cast<std::map<std::string, std::string> *>(c);
                m.emplace(n, v);
            }, &persist_props, true);
            for (auto &[k, v] : persist_props) merged.emplace(k, v); // emplace = keep existing
        }
        for (auto &[name, value] : merged) {
            if (show_context) {
                auto ctx = get_prop_context(name.c_str());
                std::printf("[%s]: [%s]\n", name.c_str(), ctx.c_str());
            } else {
                std::printf("[%s]: [%s]\n", name.c_str(), value.c_str());
            }
        }
        return 0;
    }
    if (pos.size() == 1) {
        if (show_context) {
            std::string ctx = get_prop_context(pos[0].c_str());
            std::printf("%s\n", ctx.c_str());
            return ctx.empty() ? 1 : 0;
        }
        // Rust: -P reads ONLY the persist file; -p reads runtime then falls back to persist.
        std::string val;
        if (persist_only) {
            // Read directly from the persist store by listing and looking up.
            std::map<std::string, std::string> persist_props;
            getprops([](const char *n, const char *v, void *c) {
                auto &m = *static_cast<std::map<std::string, std::string> *>(c);
                m.emplace(n, v);
            }, &persist_props, true);
            auto it = persist_props.find(pos[0]);
            if (it != persist_props.end()) val = it->second;
            if (val.empty() && persist_props.find(pos[0]) == persist_props.end()) {
                std::fprintf(stderr, "resetprop: %s not found\n", pos[0].c_str());
                return 1;
            }
            std::printf("%s\n", val.c_str());
            return 0;
        }
        val = getprop(pos[0].c_str(), persistent);
        // Distinguish a genuinely-empty property from an absent one: Rust returns
        // Some("") for an existing empty prop -> prints a blank line and exits 0.
        if (val.empty() && __system_property_find(pos[0].c_str()) == nullptr) {
            std::fprintf(stderr, "resetprop: %s not found\n", pos[0].c_str());
            return 1;
        }
        std::printf("%s\n", val.c_str());
        return 0;
    }
    if (pos.size() == 2) {
        int r = setprop(pos[0].c_str(), pos[1].c_str(), !skip_svc);
        if (r != 0) return 1;
        // Magisk semantics, mirrored from ksu_props: when -p is set AND we bypassed
        // property_service (skip_svc, or ro.* which is auto-rerouted), also persist
        // through the on-disk store, because init won't do it for us.
        bool skip = skip_svc || starts_with(pos[0], "ro.");
        if (skip && persistent && starts_with(pos[0], "persist.")) {
            if (!persist_setprop(pos[0].c_str(), pos[1].c_str())) {
                std::fprintf(stderr, "resetprop: persistent write failed\n");
                return 1;
            }
        }
        return 0;
    }

    std::fprintf(stderr, "resetprop: property name is required\n");
    return 1;
}

} // namespace

int resetprop_main(const std::vector<std::string> &args) { return run(args, 1); }

void execute_args(const std::vector<std::string> &args) {
    int code = run(args, 0);
    if (code != 0) std::exit(code);
}

void load_system_prop_file(const std::string &path) {
    load_prop_file(path.c_str(), false); // -n: direct write, skip property_service
    log::info("Loaded system.prop from {}", path);
}

} // namespace apd::resetprop
