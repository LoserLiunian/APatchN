#include "zip_util.hpp"
#include "result.hpp"

#include "miniz.h"

#include <filesystem>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace apd::zip {
namespace {

struct Archive {
    mz_zip_archive z{};
    explicit Archive(const std::string &path) {
        if (!mz_zip_reader_init_file(&z, path.c_str(), 0))
            bailf("Failed to open zip: {}", path);
    }
    ~Archive() { mz_zip_reader_end(&z); }
    Archive(const Archive &) = delete;
    Archive &operator=(const Archive &) = delete;
};

} // namespace

std::string extract_file_to_memory(const std::string &zip_path, const std::string &entry) {
    Archive a(zip_path);
    int idx = mz_zip_reader_locate_file(&a.z, entry.c_str(), nullptr, 0);
    if (idx < 0) bailf("Entry '{}' not found in {}", entry, zip_path);

    std::size_t size = 0;
    void *p = mz_zip_reader_extract_to_heap(&a.z, static_cast<mz_uint>(idx), &size, 0);
    if (!p) bailf("Failed to extract '{}' from {}", entry, zip_path);
    std::string out(static_cast<const char *>(p), size);
    mz_free(p);
    return out;
}

std::vector<std::string> list_names(const std::string &zip_path) {
    Archive a(zip_path);
    std::vector<std::string> names;
    mz_uint n = mz_zip_reader_get_num_files(&a.z);
    names.reserve(n);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&a.z, i, &st)) names.emplace_back(st.m_filename);
    }
    return names;
}

void extract_all(const std::string &zip_path, const std::string &dest_dir) {
    Archive a(zip_path);
    fs::path dest = fs::path(dest_dir);
    std::error_code ec;
    fs::create_directories(dest, ec);
    fs::path dest_abs = fs::weakly_canonical(dest, ec);

    mz_uint n = mz_zip_reader_get_num_files(&a.z);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&a.z, i, &st)) continue;

        fs::path target = dest / st.m_filename;
        // zip-slip guard: target must stay within dest. Use a path-component
        // relation (not a raw string prefix) so a sibling dir sharing a name
        // prefix — e.g. dest "/a/mod" vs entry resolving to "/a/mod-evil" —
        // cannot pass the check.
        fs::path target_abs = fs::weakly_canonical(target, ec);
        fs::path rel = target_abs.lexically_relative(dest_abs);
        if (rel.empty() || *rel.begin() == fs::path(".."))
            bailf("Unsafe zip entry escapes destination: {}", st.m_filename);

        if (mz_zip_reader_is_file_a_directory(&a.z, i)) {
            fs::create_directories(target, ec);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (!mz_zip_reader_extract_to_file(&a.z, i, target.string().c_str(), 0))
            bailf("Failed to extract '{}'", st.m_filename);

        // preserve unix permissions stored in the external attributes
        mode_t mode = static_cast<mode_t>((st.m_external_attr >> 16) & 0xFFF);
        if (mode == 0) mode = 0644;
        ::chmod(target.string().c_str(), mode);
    }
}

} // namespace apd::zip
