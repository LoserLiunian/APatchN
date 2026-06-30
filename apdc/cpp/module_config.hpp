#pragma once
// Per-module key-value config (persist + temp). Mirrors ../apd/src/module_config.rs.
// On-disk format: magic "APTM"(LE u32) | version(LE u32) | count(LE u32) |
//   count * { keyLen(LE u32) | key | valLen(LE u32) | val }.
#include <map>
#include <string>

namespace apd::module_config {

using Config = std::map<std::string, std::string>;

enum class ConfigType { Persist, Temp };

inline constexpr std::size_t MAX_CONFIG_KEY_LEN = 256;
inline constexpr std::size_t MAX_CONFIG_VALUE_LEN = 1024 * 1024; // 1 MB
inline constexpr std::size_t MAX_CONFIG_COUNT = 32;

void validate_config_key(const std::string &key);
void validate_config_value(const std::string &value);

Config load_config(const std::string &module_id, ConfigType type);
void save_config(const std::string &module_id, ConfigType type, const Config &config);

void set_config_value(const std::string &module_id, const std::string &key,
                      const std::string &value, ConfigType type);
void delete_config_value(const std::string &module_id, const std::string &key, ConfigType type);
void clear_config(const std::string &module_id, ConfigType type);

Config merge_configs(const std::string &module_id);
std::map<std::string, Config> get_all_module_configs();
void clear_all_temp_configs();
void clear_module_configs(const std::string &module_id);

} // namespace apd::module_config
