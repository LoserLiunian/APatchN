#include "properties.hpp"
#include "result.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace apd::properties {
namespace {

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\f'; }

void append_utf8(std::string &out, unsigned cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decode escapes within a key or value, advancing past a leading run already
// handled by the caller. `s` is the raw (already de-continued) segment.
std::string unescape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i >= s.size()) break;
        char e = s[i];
        switch (e) {
        case 't': out.push_back('\t'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
            unsigned cp = 0;
            int n = 0;
            for (; n < 4 && i + 1 < s.size(); ++n) {
                char h = s[i + 1];
                unsigned d;
                if (h >= '0' && h <= '9') d = static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f') d = static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') d = static_cast<unsigned>(h - 'A' + 10);
                else break;
                cp = (cp << 4) | d;
                ++i;
            }
            // A \u escape requires exactly 4 hex digits, like java-properties;
            // anything shorter is a hard error (matches the Rust crate).
            ensure(n == 4, "Malformed \\uxxxx encoding in properties");
            append_utf8(out, cp);
            break;
        }
        default: out.push_back(e); break;
        }
    }
    return out;
}

// Join physical lines that end with an odd number of backslashes.
std::vector<std::string> logical_lines(const std::string &content) {
    std::vector<std::string> phys;
    std::string cur;
    for (char c : content) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            phys.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        if (cur.back() == '\r') cur.pop_back();
        phys.push_back(std::move(cur));
    }

    std::vector<std::string> out;
    for (std::size_t i = 0; i < phys.size(); ++i) {
        std::string line = phys[i];
        // count trailing backslashes
        auto trailing_bs = [](const std::string &l) {
            std::size_t n = 0;
            for (auto it = l.rbegin(); it != l.rend() && *it == '\\'; ++it) ++n;
            return n;
        };
        while ((trailing_bs(line) % 2) == 1 && i + 1 < phys.size()) {
            line.pop_back(); // drop the continuation backslash
            std::string next = phys[++i];
            std::size_t j = 0;
            while (j < next.size() && is_ws(next[j])) ++j; // skip leading ws of next
            line += next.substr(j);
        }
        out.push_back(std::move(line));
    }
    return out;
}

} // namespace

std::map<std::string, std::string> parse(const std::string &content) {
    std::map<std::string, std::string> result;
    for (const std::string &line : logical_lines(content)) {
        std::size_t i = 0;
        while (i < line.size() && is_ws(line[i])) ++i;
        if (i >= line.size()) continue;
        if (line[i] == '#' || line[i] == '!') continue;

        // key: up to first unescaped '=', ':' or whitespace
        std::string raw_key;
        for (; i < line.size(); ++i) {
            char c = line[i];
            if (c == '\\' && i + 1 < line.size()) {
                raw_key.push_back(c);
                raw_key.push_back(line[++i]);
                continue;
            }
            if (c == '=' || c == ':' || is_ws(c)) break;
            raw_key.push_back(c);
        }
        // skip whitespace, then an optional single '=' or ':', then whitespace
        while (i < line.size() && is_ws(line[i])) ++i;
        if (i < line.size() && (line[i] == '=' || line[i] == ':')) {
            ++i;
            while (i < line.size() && is_ws(line[i])) ++i;
        }
        std::string raw_val = line.substr(i);

        result[unescape(raw_key)] = unescape(raw_val);
    }
    return result;
}

std::map<std::string, std::string> parse_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) bailf("Failed to read {}", path);
    std::stringstream ss;
    ss << in.rdbuf();
    return parse(ss.str());
}

} // namespace apd::properties
