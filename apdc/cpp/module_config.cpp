#include "module_config.hpp"

#include "defs.hpp"
#include "logging.hpp"
#include "result.hpp"
#include "utils.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace apd::module_config {
namespace {

constexpr uint32_t MAGIC = 0x4150544D; // "APTM"
constexpr uint32_t VERSION = 1;

const char *type_filename(ConfigType t) {
    return t == ConfigType::Persist ? defs::PERSIST_CONFIG_NAME : defs::TEMP_CONFIG_NAME;
}

fs::path config_dir(const std::string &module_id) {
    return fs::path(defs::MODULE_CONFIG_DIR) / module_id;
}

fs::path config_path(const std::string &module_id, ConfigType type) {
    return config_dir(module_id) / type_filename(type);
}

void validate_config_count(const Config &config) {
    ensuref(config.size() <= MAX_CONFIG_COUNT, "Too many config entries: {} (max: {})",
            config.size(), MAX_CONFIG_COUNT);
}

void put_u32_le(std::string &buf, uint32_t v) {
    char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF),
                 char((v >> 24) & 0xFF)};
    buf.append(b, 4);
}

uint32_t get_u32_le(const unsigned char *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Strict UTF-8 validation, matching Rust's String::from_utf8 (rejects overlong
// encodings, surrogates, and code points > U+10FFFF).
bool is_valid_utf8(const std::string &s) {
    std::size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t extra;
        unsigned cp;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; }
        else return false;
        if (i + extra >= n) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += extra + 1;
    }
    return true;
}

} // namespace

void validate_config_key(const std::string &key) {
    if (key.empty()) bail("Config key cannot be empty");
    ensuref(key.size() <= MAX_CONFIG_KEY_LEN, "Config key too long: {} bytes (max: {})",
            key.size(), MAX_CONFIG_KEY_LEN);

    // ^[a-zA-Z][a-zA-Z0-9._-]+$  (min length 2)
    auto is_first = [](unsigned char c) { return std::isalpha(c) != 0; };
    auto is_rest = [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-';
    };
    bool ok = key.size() >= 2 && is_first(static_cast<unsigned char>(key[0]));
    for (std::size_t i = 1; ok && i < key.size(); ++i)
        ok = is_rest(static_cast<unsigned char>(key[i]));
    ensuref(ok, "Invalid config key: '{}'. Must match /^[a-zA-Z][a-zA-Z0-9._-]+$/", key);
}

void validate_config_value(const std::string &value) {
    ensuref(value.size() <= MAX_CONFIG_VALUE_LEN, "Config value too long: {} bytes (max: {})",
            value.size(), MAX_CONFIG_VALUE_LEN);
    ensure(is_valid_utf8(value), "Config value is not valid UTF-8");
}

void validate_config_key_utf8(const std::string &key) {
    ensure(is_valid_utf8(key), "Config key is not valid UTF-8");
}

Config load_config(const std::string &module_id, ConfigType type) {
    fs::path path = config_path(module_id, type);
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        log::debug("Config file not found: {}", path.string());
        return {};
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) bailf("Failed to open config file: {}", path.string());
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto *p = reinterpret_cast<const unsigned char *>(data.data());
    std::size_t n = data.size();
    std::size_t off = 0;

    auto need = [&](std::size_t want, const char *what) {
        ensuref(off + want <= n, "Failed to read {}", what);
    };

    need(4, "magic");
    uint32_t magic = get_u32_le(p + off);
    off += 4;
    ensuref(magic == MAGIC, "Invalid config magic: expected 0x{:08x}, got 0x{:08x}", MAGIC, magic);

    need(4, "version");
    uint32_t version = get_u32_le(p + off);
    off += 4;
    ensuref(version == VERSION, "Unsupported config version: expected {}, got {}", VERSION, version);

    need(4, "count");
    uint32_t count = get_u32_le(p + off);
    off += 4;

    Config config;
    for (uint32_t i = 0; i < count; ++i) {
        need(4, "key length");
        uint32_t key_len = get_u32_le(p + off);
        off += 4;
        need(key_len, "key data");
        std::string key(reinterpret_cast<const char *>(p + off), key_len);
        off += key_len;
        ensuref(is_valid_utf8(key), "Config key in {} is not valid UTF-8", path.string());

        need(4, "value length");
        uint32_t val_len = get_u32_le(p + off);
        off += 4;
        need(val_len, "value data");
        std::string val(reinterpret_cast<const char *>(p + off), val_len);
        off += val_len;
        ensuref(is_valid_utf8(val), "Config value for key '{}' in {} is not valid UTF-8", key,
                path.string());

        config.emplace(std::move(key), std::move(val));
    }

    log::debug("Loaded {} entries from {}", config.size(), path.string());
    return config;
}

void save_config(const std::string &module_id, ConfigType type, const Config &config) {
    validate_config_count(config);
    for (const auto &[key, value] : config) {
        validate_config_key(key);
        validate_config_value(value);
    }

    utils::ensure_dir_exists(config_dir(module_id).string());

    fs::path path = config_path(module_id, type);
    fs::path tmp = path;
    tmp += ".tmp";

    std::string buf;
    put_u32_le(buf, MAGIC);
    put_u32_le(buf, VERSION);
    put_u32_le(buf, static_cast<uint32_t>(config.size()));
    for (const auto &[key, value] : config) {
        put_u32_le(buf, static_cast<uint32_t>(key.size()));
        buf.append(key);
        put_u32_le(buf, static_cast<uint32_t>(value.size()));
        buf.append(value);
    }

    int fd = ::open(tmp.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) bailf("Failed to create temp config file: {}", tmp.string());
    const char *data = buf.data();
    std::size_t left = buf.size();
    while (left > 0) {
        ssize_t w = ::write(fd, data, left);
        if (w < 0) {
            ::close(fd);
            bailf("Failed to write config file: {}", tmp.string());
        }
        data += w;
        left -= static_cast<std::size_t>(w);
    }
    // ENOSPC/EIO mid-fsync must abort BEFORE rename, or we publish a torn write.
    if (::fsync(fd) != 0) {
        int e = errno;
        ::close(fd);
        bailf("Failed to fsync temp config file {}: errno={}", tmp.string(), e);
    }
    if (::close(fd) != 0) {
        int e = errno;
        bailf("Failed to close temp config file {}: errno={}", tmp.string(), e);
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) bailf("Failed to rename config file: {} -> {}", tmp.string(), path.string());

    log::debug("Saved {} entries to {}", config.size(), path.string());
}

void set_config_value(const std::string &module_id, const std::string &key,
                      const std::string &value, ConfigType type) {
    validate_config_key(key);
    validate_config_value(value);

    Config config = load_config(module_id, type);
    config[key] = value;
    save_config(module_id, type, config);
}

void delete_config_value(const std::string &module_id, const std::string &key, ConfigType type) {
    Config config = load_config(module_id, type);
    if (config.erase(key) == 0) bailf("Key '{}' not found in config", key);
    save_config(module_id, type, config);
}

void clear_config(const std::string &module_id, ConfigType type) {
    fs::path path = config_path(module_id, type);
    std::error_code ec;
    if (fs::exists(path, ec)) {
        fs::remove(path, ec);
        if (ec) bailf("Failed to remove config file: {}", path.string());
        log::debug("Cleared config: {}", path.string());
    }
}

Config merge_configs(const std::string &module_id) {
    Config merged;
    try {
        merged = load_config(module_id, ConfigType::Persist);
    } catch (const std::exception &e) {
        log::warn("Failed to load persist config for module '{}': {}", module_id, e.what());
    }
    try {
        Config temp = load_config(module_id, ConfigType::Temp);
        for (auto &[k, v] : temp) merged[k] = v; // temp overrides persist
    } catch (const std::exception &e) {
        log::warn("Failed to load temp config for module '{}': {}", module_id, e.what());
    }
    return merged;
}

std::map<std::string, Config> get_all_module_configs() {
    std::map<std::string, Config> all;
    fs::path root(defs::MODULE_CONFIG_DIR);
    std::error_code ec;
    if (!fs::exists(root, ec)) return all;

    for (const auto &entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        std::string module_id = entry.path().filename().string();
        try {
            Config config = merge_configs(module_id);
            if (!config.empty()) all.emplace(module_id, std::move(config));
        } catch (const std::exception &e) {
            log::warn("Failed to load config for module '{}': {}", module_id, e.what());
        }
    }
    return all;
}

void clear_all_temp_configs() {
    fs::path root(defs::MODULE_CONFIG_DIR);
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        log::debug("Config directory does not exist, nothing to clear");
        return;
    }

    int cleared = 0;
    for (const auto &entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        fs::path temp = entry.path() / defs::TEMP_CONFIG_NAME;
        if (fs::exists(temp, ec)) {
            std::error_code rec;
            fs::remove(temp, rec);
            if (rec)
                log::warn("Failed to clear temp config {}: {}", temp.string(), rec.message());
            else {
                log::debug("Cleared temp config: {}", temp.string());
                ++cleared;
            }
        }
    }
    if (cleared > 0) log::debug("Cleared {} temp config file(s)", cleared);
}

void clear_module_configs(const std::string &module_id) {
    fs::path dir = config_dir(module_id);
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        fs::remove_all(dir, ec);
        if (ec) bailf("Failed to remove config directory: {}", dir.string());
        log::debug("Cleared all configs for module: {}", module_id);
    }
}

} // namespace apd::module_config
