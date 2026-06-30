#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <utils.hpp>
#include <magiskpolicy.hpp>

#include "sepolicy.hpp"

// SELinux policy paths (originally from Magisk's selinux.hpp).
#define PLAT_POLICY_DIR   "/system/etc/selinux/"
#define VEND_POLICY_DIR   "/vendor/etc/selinux/"
#define PROD_POLICY_DIR   "/product/etc/selinux/"
#define ODM_POLICY_DIR    "/odm/etc/selinux/"
#define SYSEXT_POLICY_DIR "/system_ext/etc/selinux/"
#define SPLIT_PLAT_CIL    PLAT_POLICY_DIR "plat_sepolicy.cil"
#define SELINUX_MNT       "/sys/fs/selinux"
#define SELINUX_VERSION   SELINUX_MNT "/policyvers"

#define SHALEN 64
static bool cmp_sha256(const char *a, const char *b) {
    char id_a[SHALEN] = {0};
    char id_b[SHALEN] = {0};
    if (int fd = xopen(a, O_RDONLY | O_CLOEXEC); fd >= 0) {
        xread(fd, id_a, SHALEN);
        close(fd);
    } else {
        return false;
    }

    if (int fd = xopen(b, O_RDONLY | O_CLOEXEC); fd >= 0) {
        xread(fd, id_b, SHALEN);
        close(fd);
    } else {
        return false;
    }
    LOGD("%s=[%.*s]\n", a, SHALEN, id_a);
    LOGD("%s=[%.*s]\n", b, SHALEN, id_b);
    return memcmp(id_a, id_b, SHALEN) == 0;
}

static bool check_precompiled(const char *precompiled) {
    bool ok = false;
    const char *actual_sha;
    char compiled_sha[128];

    actual_sha = PLAT_POLICY_DIR "plat_and_mapping_sepolicy.cil.sha256";
    if (access(actual_sha, R_OK) == 0) {
        ok = true;
        sprintf(compiled_sha, "%s.plat_and_mapping.sha256", precompiled);
        if (!cmp_sha256(actual_sha, compiled_sha))
            return false;
    }

    actual_sha = PLAT_POLICY_DIR "plat_sepolicy_and_mapping.sha256";
    if (access(actual_sha, R_OK) == 0) {
        ok = true;
        sprintf(compiled_sha, "%s.plat_sepolicy_and_mapping.sha256", precompiled);
        if (!cmp_sha256(actual_sha, compiled_sha))
            return false;
    }

    actual_sha = PROD_POLICY_DIR "product_sepolicy_and_mapping.sha256";
    if (access(actual_sha, R_OK) == 0) {
        ok = true;
        sprintf(compiled_sha, "%s.product_sepolicy_and_mapping.sha256", precompiled);
        if (!cmp_sha256(actual_sha, compiled_sha) != 0)
            return false;
    }

    actual_sha = SYSEXT_POLICY_DIR "system_ext_sepolicy_and_mapping.sha256";
    if (access(actual_sha, R_OK) == 0) {
        ok = true;
        sprintf(compiled_sha, "%s.system_ext_sepolicy_and_mapping.sha256", precompiled);
        if (!cmp_sha256(actual_sha, compiled_sha) != 0)
            return false;
    }

    return ok;
}

sepolicy *sepolicy::from_file(const char *file) {
    LOGD("Load policy from: %s\n", file);

    policy_file_t pf;
    policy_file_init(&pf);
    auto fp = xopen_file(file, "re");
    pf.fp = fp.get();
    pf.type = PF_USE_STDIO;

    auto db = static_cast<policydb_t *>(xmalloc(sizeof(policydb_t)));
    if (policydb_init(db) || policydb_read(db, &pf, 0)) {
        LOGE("Fail to load policy from %s\n", file);
        free(db);
        return nullptr;
    }

    auto sepol = new sepolicy();
    sepol->db = db;
    return sepol;
}

sepolicy *sepolicy::compile_split() {
    // CIL split-policy compilation is unsupported in apd (avoids vendoring
    // libsepol/cil + flex/bison). APatch devices use the monolithic policy at
    // /sys/fs/selinux/policy. ponytail: add CIL if a split-only device needs it.
    LOGE("split policy compilation is not supported\n");
    return nullptr;
}

sepolicy *sepolicy::from_split() {
    const char *odm_pre = ODM_POLICY_DIR "precompiled_sepolicy";
    const char *vend_pre = VEND_POLICY_DIR "precompiled_sepolicy";
    if (access(odm_pre, R_OK) == 0 && check_precompiled(odm_pre))
        return sepolicy::from_file(odm_pre);
    else if (access(vend_pre, R_OK) == 0 && check_precompiled(vend_pre))
        return sepolicy::from_file(vend_pre);
    else
        return sepolicy::compile_split();
}

sepolicy::~sepolicy() {
    policydb_destroy(db);
    free(db);
}

bool sepolicy::to_file(const char *file) {
    char *data = nullptr;
    size_t len = 0;

    /* No partial writes are allowed to /sys/fs/selinux/load, thus the reason why we
     * first dump everything into memory, then directly call write system call.
     * open_memstream gives us a FILE* backed by a growable heap buffer. */
    FILE *fp = open_memstream(&data, &len);
    if (fp == nullptr) {
        LOGE("Fail to open memstream\n");
        return false;
    }

    policy_file_t pf;
    policy_file_init(&pf);
    pf.type = PF_USE_STDIO;
    pf.fp = fp;
    if (policydb_write(db, &pf)) {
        LOGE("Fail to create policy image\n");
        fclose(fp);
        free(data);
        return false;
    }
    fclose(fp); // finalizes data/len
    run_finally fin([=] { free(data); });

    int fd = xopen(file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    xwrite(fd, data, len);

    close(fd);
    return true;
}
