#pragma once
// Root-list package config (CSV) + sync with system packages.list.
// Mirrors ../apd/src/package.rs.
#include <string>
#include <vector>

namespace apd::package {

struct PackageConfig {
    std::string pkg;
    int exclude = 0;
    int allow = 0;
    int uid = 0;
    int to_uid = 0;
    std::string sctx;
};

// Reads /data/adb/ap/package_config (retries up to 5x). Empty vec on failure.
std::vector<PackageConfig> read_ap_package_config();

// Atomically writes the CSV (temp + rename, retries up to 5x). Throws on failure.
void write_ap_package_config(const std::vector<PackageConfig> &configs);

// Reconcile package_config with /data/system/packages.list (drop uninstalled,
// refresh appid part of uid). Throws on persistent failure.
void synchronize_package_uid();

} // namespace apd::package
