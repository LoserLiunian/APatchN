#include "package.hpp"
#include "logging.hpp"
#include "result.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>

namespace apd::package {
namespace {

constexpr const char *CONFIG_PATH = "/data/adb/ap/package_config";
constexpr const char *CONFIG_TMP = "/data/adb/ap/package_config.tmp";
constexpr int MAX_RETRY = 5;

std::vector<std::string> read_lines(std::istream &in) {
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> parse_csv_line(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inq) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    inq = false;
                }
            } else {
                cur.push_back(c);
            }
        } else if (c == '"') {
            inq = true;
        } else if (c == ',') {
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(std::move(cur));
    return out;
}

std::string csv_field(const std::string &s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out += "\"";
    return out;
}

bool to_int(const std::string &s, int &out) {
    try {
        std::size_t pos = 0;
        out = std::stoi(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

std::vector<std::string> split_ws(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string w;
    while (iss >> w) out.push_back(w);
    return out;
}

} // namespace

std::vector<PackageConfig> read_ap_package_config() {
    for (int attempt = 0; attempt < MAX_RETRY; ++attempt) {
        std::ifstream file(CONFIG_PATH);
        if (!file) {
            log::warn("Error opening file: {}", CONFIG_PATH);
            ::sleep(1);
            continue;
        }
        std::vector<std::string> lines = read_lines(file);
        if (lines.empty()) return {}; // header-less/empty == no records

        std::vector<std::string> header = parse_csv_line(lines[0]);
        std::map<std::string, std::size_t> col;
        for (std::size_t i = 0; i < header.size(); ++i) col[header[i]] = i;

        auto has = [&](const char *k) { return col.count(k) != 0; };
        // Match Rust csv::Reader: a malformed header (post-crash partial write,
        // missing columns) is a hard error — refuse to fall back to positional
        // mapping that could mis-route uids/scontexts.
        if (!(has("pkg") && has("exclude") && has("allow") && has("uid") &&
              has("to_uid") && has("sctx"))) {
            log::warn("package_config: malformed header, refusing to parse");
            ::sleep(1);
            continue;
        }

        std::vector<PackageConfig> configs;
        bool success = true;
        for (std::size_t li = 1; li < lines.size() && success; ++li) {
            if (lines[li].empty()) continue;
            std::vector<std::string> f = parse_csv_line(lines[li]);
            auto field = [&](const char *name) -> const std::string & {
                static const std::string kEmpty;
                auto it = col.find(name);
                if (it != col.end() && it->second < f.size()) return f[it->second];
                return kEmpty;
            };
            PackageConfig c;
            c.pkg = field("pkg");
            c.sctx = field("sctx");
            if (!to_int(field("exclude"), c.exclude) || !to_int(field("allow"), c.allow) ||
                !to_int(field("uid"), c.uid) || !to_int(field("to_uid"), c.to_uid)) {
                log::warn("Error deserializing record: {}", lines[li]);
                success = false;
                break;
            }
            configs.push_back(std::move(c));
        }
        if (success) return configs;
        ::sleep(1);
    }
    return {};
}

void write_ap_package_config(const std::vector<PackageConfig> &configs) {
    for (int attempt = 0; attempt < MAX_RETRY; ++attempt) {
        std::ofstream file(CONFIG_TMP, std::ios::trunc);
        if (!file) {
            log::warn("Error creating temp file: {}", CONFIG_TMP);
            ::sleep(1);
            continue;
        }
        // Match Rust csv::Writer: the header is written lazily on the first
        // record, so an empty list yields a completely empty file.
        // Rust csv::Writer defaults to CRLF terminators; match for byte parity.
        if (!configs.empty()) file << "pkg,exclude,allow,uid,to_uid,sctx\r\n";
        for (const auto &c : configs) {
            file << csv_field(c.pkg) << ',' << c.exclude << ',' << c.allow << ',' << c.uid << ','
                 << c.to_uid << ',' << csv_field(c.sctx) << "\r\n";
        }
        file.flush();
        if (!file) {
            log::warn("Error flushing writer");
            ::sleep(1);
            continue;
        }
        file.close();
        std::error_code ec;
        std::filesystem::rename(CONFIG_TMP, CONFIG_PATH, ec);
        if (ec) {
            log::warn("Error renaming temp file: {}", ec.message());
            ::sleep(1);
            continue;
        }
        return;
    }
    bail("Failed after max retries");
}

void synchronize_package_uid() {
    log::info("[synchronize_package_uid] Start synchronizing root list with system packages...");

    for (int attempt = 0; attempt < MAX_RETRY; ++attempt) {
        std::ifstream in("/data/system/packages.list");
        if (!in) {
            log::warn("Error reading packages.list");
            ::sleep(1);
            continue;
        }
        std::vector<std::string> lines = read_lines(in);

        std::vector<PackageConfig> configs = read_ap_package_config();

        std::vector<std::string> system_packages;
        for (const auto &line : lines) {
            auto w = split_ws(line);
            if (!w.empty()) system_packages.push_back(w[0]);
        }
        auto in_system = [&](const std::string &pkg) {
            for (const auto &p : system_packages)
                if (p == pkg) return true;
            return false;
        };

        std::size_t original_len = configs.size();
        std::vector<PackageConfig> kept;
        for (auto &c : configs)
            if (in_system(c.pkg)) kept.push_back(std::move(c));
        configs = std::move(kept);
        std::size_t removed_count = original_len - configs.size();
        if (removed_count > 0)
            log::info("Removed {} uninstalled package configurations", removed_count);

        bool updated = false;
        for (const auto &line : lines) {
            auto words = split_ws(line);
            if (words.size() < 2) continue;
            const std::string &pkg_name = words[0];
            int uid = 0;
            if (!to_int(words[1], uid)) {
                log::warn("Error parsing uid: {}", words[1]);
                continue;
            }
            for (auto &c : configs) {
                if (c.pkg != pkg_name) continue;
                if (c.uid % 100000 != uid % 100000) {
                    int new_uid = c.uid / 100000 * 100000 + uid % 100000;
                    log::info("Updating uid for package {}: {} -> {}", pkg_name, c.uid, new_uid);
                    c.uid = new_uid;
                    updated = true;
                }
            }
        }

        if (updated || removed_count > 0) write_ap_package_config(configs);
        return;
    }
    bail("Failed after max retries");
}

} // namespace apd::package
