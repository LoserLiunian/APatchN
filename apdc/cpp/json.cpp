#include "json.hpp"

#include <format>

namespace apd::json {

std::string escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20)
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            else
                out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string to_pretty(const std::vector<std::map<std::string, std::string>> &arr) {
    if (arr.empty()) return "[]";

    std::string out = "[\n";
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto &obj = arr[i];
        if (obj.empty()) {
            out += "  {}";
        } else {
            out += "  {\n";
            std::size_t j = 0;
            for (const auto &[k, v] : obj) {
                out += std::format("    \"{}\": \"{}\"", escape(k), escape(v));
                out += (++j < obj.size()) ? ",\n" : "\n";
            }
            out += "  }";
        }
        out += (i + 1 < arr.size()) ? ",\n" : "\n";
    }
    out += "]";
    return out;
}

} // namespace apd::json
