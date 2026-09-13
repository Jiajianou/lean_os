#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "caps.h"
#include "../user_space/library/os_package.h"
#include "../user_space/library/sha256.h"

#define MAX_STAGE_FILES OSP_MAX_FILES

typedef struct {
    char     rel[OSP_MAX_PATH];
    char     host[4096];
    uint64_t size;
    int      is_symlink;
    int      is_exec;
    char     link_target[OSP_MAX_PATH];
} entry_t;

static entry_t entries[MAX_STAGE_FILES];
static int entry_count;

static void die(const char *fmt, ...) {
    va_list ap;
    __builtin_va_start(ap, fmt);
    fprintf(stderr, "os-pkg: ");
    vfprintf(stderr, fmt, ap);
    __builtin_va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static unsigned char *read_whole(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    unsigned char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len_out = (size_t)n;
    return buf;
}

static int cmp_names(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void walk(const char *host_directory, const char *rel_prefix) {
    DIR *d = opendir(host_directory);
    if (!d) {
        die("cannot open %s: %s", host_directory, strerror(errno));
    }
    char *names[MAX_STAGE_FILES];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (n >= MAX_STAGE_FILES) {
            die("more than %d entries in %s", MAX_STAGE_FILES, host_directory);
        }
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_names);

    for (int i = 0; i < n; i++) {
        char host[4096];
        char rel[OSP_MAX_PATH];
        snprintf(host, sizeof(host), "%s/%s", host_directory, names[i]);
        if (rel_prefix[0]) {
            if ((int)snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, names[i]) >= (int)sizeof(rel)) {
                die("path too long inside the package: %s/%s", rel_prefix, names[i]);
            }
        } else {
            if ((int)snprintf(rel, sizeof(rel), "%s", names[i]) >= (int)sizeof(rel)) {
                die("path too long inside the package: %s", names[i]);
            }
        }

        struct stat st;
        if (lstat(host, &st) != 0) {
            die("cannot stat %s: %s", host, strerror(errno));
        }

        if (S_ISDIR(st.st_mode)) {
            walk(host, rel);
            continue;
        }
        if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
            die("%s is neither a regular file nor a symlink - a package "
                "holds files, and a device node or socket in one is a "
                "question this format refuses rather than answers", host);
        }
        if (os_package_check_path(rel) != OSP_OK) {
            die("%s is not a path a package may contain", rel);
        }
        if (entry_count >= MAX_STAGE_FILES) {
            die("more than %d files in the package", MAX_STAGE_FILES);
        }
        entry_t *e = &entries[entry_count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->rel, sizeof(e->rel), "%s", rel);
        snprintf(e->host, sizeof(e->host), "%s", host);
        if (S_ISLNK(st.st_mode)) {
            ssize_t r = readlink(host, e->link_target, sizeof(e->link_target) - 1);
            if (r < 0) {
                die("cannot read the link %s: %s", host, strerror(errno));
            }
            e->link_target[r] = '\0';
            e->is_symlink = 1;
            e->size = (uint64_t)r;
        } else {
            e->size = (uint64_t)st.st_size;
            e->is_exec = (st.st_mode & S_IXUSR) != 0;
        }
    }
    for (int i = 0; i < n; i++) {
        free(names[i]);
    }
}

static void wr32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void wr64(unsigned char *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static int command_build(const char *manifest_path, const char *stage, const char *out_path) {
    size_t meta_length = 0;
    unsigned char *meta = read_whole(manifest_path, &meta_length);
    if (!meta) {
        die("cannot read the manifest %s: %s", manifest_path, strerror(errno));
    }

    while ((meta_length & 7u) != 0) {
        meta = realloc(meta, meta_length + 2);
        if (!meta) {
            die("out of memory padding the manifest");
        }
        meta[meta_length++] = '\n';
        meta[meta_length] = '\0';
    }

    osp_manifest_t man;
    int rc = os_package_parse_manifest((const char *)meta, meta_length, &man);
    if (rc != OSP_OK) {
        die("%s: %s", manifest_path, osp_strerror(rc));
    }
    int unknown = 0;
    uint32_t caps = os_package_caps_from_names(man.caps, &unknown);
    if (unknown) {
        die("%s: 'caps:' names a capability this OS does not have - see "
            "system_api/include/caps.h for the list", manifest_path);
    }
    if (caps & ~(uint32_t)CAP_PKG_MAX) {
        char names[OSP_MAX_TEXT];
        os_package_caps_to_names(caps & ~(uint32_t)CAP_PKG_MAX, names, sizeof(names));
        die("%s: a package may not ask for '%s' - see CAP_PKG_MAX in "
            "system_api/include/caps.h for why", manifest_path, names);
    }

    entry_count = 0;
    walk(stage, "");
    if (entry_count == 0) {
        die("%s is empty - a package with no files is not a package", stage);
    }

    uint64_t payload_bytes = 0;
    for (int i = 0; i < entry_count; i++) {
        payload_bytes += entries[i].size;
    }
    if (payload_bytes > OSP_MAX_PAYLOAD) {
        die("the payload is %llu bytes, over the %u-byte ceiling this "
            "machine will read", (unsigned long long)payload_bytes,
            (unsigned)OSP_MAX_PAYLOAD);
    }

    unsigned char *payload = malloc(payload_bytes ? (size_t)payload_bytes : 1);
    osp_file_t *table = calloc((size_t)entry_count, sizeof(osp_file_t));
    if (!payload || !table) {
        die("out of memory laying out %d files", entry_count);
    }

    uint64_t off = 0;
    for (int i = 0; i < entry_count; i++) {
        entry_t *e = &entries[i];
        const unsigned char *data;
        if (e->is_symlink) {
            data = (const unsigned char *)e->link_target;
        } else {
            size_t got = 0;
            unsigned char *buf = read_whole(e->host, &got);
            if (!buf) {
                die("cannot read %s: %s", e->host, strerror(errno));
            }
            if ((uint64_t)got != e->size) {
                die("%s changed size while the package was being built", e->host);
            }
            data = buf;
        }
        memcpy(payload + off, data, (size_t)e->size);
        if (!e->is_symlink) {
            free((void *)data);
        }

        osp_file_t *f = &table[i];
        memset(f, 0, sizeof(*f));
        snprintf(f->path, sizeof(f->path), "%s", e->rel);
        f->size = e->size;
        f->offset = off;
        f->flags = (e->is_symlink ? OSP_F_SYMLINK : 0u) | (e->is_exec ? OSP_F_EXEC : 0u);
        sha256(payload + off, (size_t)e->size, f->sha256);
        off += e->size;
    }

    size_t table_bytes = (size_t)entry_count * OSP_FILE_BYTES;
    unsigned char *table_raw = calloc(table_bytes, 1);
    if (!table_raw) {
        die("out of memory");
    }
    for (int i = 0; i < entry_count; i++) {
        unsigned char *p = table_raw + (size_t)i * OSP_FILE_BYTES;
        memcpy(p, table[i].path, OSP_MAX_PATH);
        wr64(p + OSP_MAX_PATH, table[i].size);
        wr64(p + OSP_MAX_PATH + 8, table[i].offset);
        wr32(p + OSP_MAX_PATH + 16, table[i].flags);
        wr32(p + OSP_MAX_PATH + 20, 0);
        memcpy(p + OSP_MAX_PATH + 24, table[i].sha256, SHA256_DIGEST_BYTES);
    }

    sha256_t body;
    sha256_init(&body);
    sha256_update(&body, meta, meta_length);
    sha256_update(&body, table_raw, table_bytes);
    sha256_update(&body, payload, (size_t)payload_bytes);
    uint8_t body_digest[SHA256_DIGEST_BYTES];
    sha256_final(&body, body_digest);

    unsigned char header[OSP_HEADER_BYTES];
    memset(header, 0, sizeof(header));
    header[0] = OSP_MAGIC0; header[1] = OSP_MAGIC1;
    header[2] = OSP_MAGIC2; header[3] = OSP_MAGIC3;
    header[4] = OSP_MAGIC4; header[5] = OSP_MAGIC5;
    header[6] = OSP_MAGIC6; header[7] = OSP_MAGIC7;
    wr32(header + 8, 1);
    wr32(header + 12, (uint32_t)meta_length);
    wr32(header + 16, (uint32_t)entry_count);
    wr32(header + 20, 0);
    wr64(header + 24, payload_bytes);
    memcpy(header + 32, body_digest, SHA256_DIGEST_BYTES);

    FILE *o = fopen(out_path, "wb");
    if (!o) {
        die("cannot write %s: %s", out_path, strerror(errno));
    }
    if (fwrite(header, 1, sizeof(header), o) != sizeof(header) ||
        fwrite(meta, 1, meta_length, o) != meta_length ||
        fwrite(table_raw, 1, table_bytes, o) != table_bytes ||
        (payload_bytes && fwrite(payload, 1, (size_t)payload_bytes, o) != payload_bytes)) {
        die("short write to %s", out_path);
    }
    fclose(o);

    size_t back_length = 0;
    unsigned char *back = read_whole(out_path, &back_length);
    if (!back) {
        die("cannot read back %s", out_path);
    }
    osp_t pkg;
    rc = os_package_open(back, back_length, &pkg);
    if (rc != OSP_OK) {
        die("the package just written does not verify: %s", osp_strerror(rc));
    }

    char hex[65];
    sha256_hex(body_digest, hex);
    printf("os-pkg: %s-%s  %d files, %llu bytes payload, %zu bytes total\n",
           man.name, man.version, entry_count,
           (unsigned long long)payload_bytes, back_length);
    printf("os-pkg: body sha256 %s\n", hex);
    if (caps) {
        char names[OSP_MAX_TEXT];
        os_package_caps_to_names(caps, names, sizeof(names));
        printf("os-pkg: asks for %s\n", names);
    }
    free(back);
    free(payload);
    free(table);
    free(table_raw);
    free(meta);
    return 0;
}

static int load(const char *path, unsigned char **bytes, size_t *len, osp_t *pkg) {
    *bytes = read_whole(path, len);
    if (!*bytes) {
        fprintf(stderr, "os-pkg: cannot read %s: %s\n", path, strerror(errno));
        return 2;
    }
    int rc = os_package_open(*bytes, *len, pkg);
    if (rc != OSP_OK) {
        fprintf(stderr, "os-pkg: %s: %s\n", path, osp_strerror(rc));
        return 1;
    }
    return 0;
}

static int command_info(const char *path) {
    unsigned char *bytes;
    size_t len;
    osp_t pkg;
    int rc = load(path, &bytes, &len, &pkg);
    if (rc) {
        return rc;
    }
    printf("name:     %s\n", pkg.manifest.name);
    printf("version:  %s\n", pkg.manifest.version);
    printf("summary:  %s\n", pkg.manifest.summary);
    if (pkg.manifest.license[0])  printf("license:  %s\n", pkg.manifest.license);
    if (pkg.manifest.source[0])   printf("source:   %s\n", pkg.manifest.source);
    if (pkg.manifest.requires[0]) printf("requires: %s\n", pkg.manifest.requires);
    if (pkg.manifest.provides[0]) printf("provides: %s\n", pkg.manifest.provides);
    printf("caps:     %s\n", pkg.manifest.caps[0] ? pkg.manifest.caps : "(none)");
    printf("files:    %u\n", pkg.file_count);
    for (uint32_t i = 0; i < pkg.file_count; i++) {
        char hex[65];
        sha256_hex(pkg.files[i].sha256, hex);
        printf("  %c%c %10llu  %.16s  %s\n",
               (pkg.files[i].flags & OSP_F_SYMLINK) ? 'l' : '-',
               (pkg.files[i].flags & OSP_F_EXEC) ? 'x' : '-',
               (unsigned long long)pkg.files[i].size, hex, pkg.files[i].path);
    }
    free(bytes);
    return 0;
}

static int command_verify(const char *path) {
    unsigned char *bytes;
    size_t len;
    osp_t pkg;
    int rc = load(path, &bytes, &len, &pkg);
    if (rc) {
        return rc;
    }
    printf("os-pkg: %s-%s verifies (%u files)\n",
           pkg.manifest.name, pkg.manifest.version, pkg.file_count);
    free(bytes);
    return 0;
}

static void mkdir_p(const char *path) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static int command_extract(const char *path, const char *dir) {
    unsigned char *bytes;
    size_t len;
    osp_t pkg;
    int rc = load(path, &bytes, &len, &pkg);
    if (rc) {
        return rc;
    }
    mkdir_p(dir);
    for (uint32_t i = 0; i < pkg.file_count; i++) {
        char out[4096];
        snprintf(out, sizeof(out), "%s/%s", dir, pkg.files[i].path);
        char parent[4096];
        snprintf(parent, sizeof(parent), "%s", out);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            mkdir_p(parent);
        }
        const uint8_t *data = os_package_file_data(&pkg, i);
        if (pkg.files[i].flags & OSP_F_SYMLINK) {
            char target[OSP_MAX_PATH + 1];
            memcpy(target, data, (size_t)pkg.files[i].size);
            target[pkg.files[i].size] = '\0';
            unlink(out);
            if (symlink(target, out) != 0) {
                die("cannot create the link %s: %s", out, strerror(errno));
            }
            continue;
        }
        FILE *f = fopen(out, "wb");
        if (!f) {
            die("cannot write %s: %s", out, strerror(errno));
        }
        if (pkg.files[i].size &&
            fwrite(data, 1, (size_t)pkg.files[i].size, f) != pkg.files[i].size) {
            die("short write to %s", out);
        }
        fclose(f);
        if (pkg.files[i].flags & OSP_F_EXEC) {
            chmod(out, 0755);
        }
    }
    printf("os-pkg: %u files into %s\n", pkg.file_count, dir);
    free(bytes);
    return 0;
}

static int command_index(const char *repo) {
    DIR *d = opendir(repo);
    if (!d) {
        die("cannot open %s: %s", repo, strerror(errno));
    }
    char *names[MAX_STAGE_FILES];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l > 4 && strcmp(de->d_name + l - 4, ".osp") == 0) {
            names[n++] = strdup(de->d_name);
        }
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_names);

    char out_path[4096];
    char temporary_path[4096];
    snprintf(out_path, sizeof(out_path), "%s/index", repo);
    snprintf(temporary_path, sizeof(temporary_path), "%s/index.new", repo);
    FILE *o = fopen(temporary_path, "wb");
    if (!o) {
        die("cannot write %s: %s", temporary_path, strerror(errno));
    }
    fprintf(o, "# lean_os package index - written by tools/os-pkg.c, M111.\n");
    fprintf(o, "# Each stanza is one package. 'sha256' is over the whole .osp\n");
    fprintf(o, "# file, so `os install` can refuse a bad archive before it\n");
    fprintf(o, "# reads anything out of it.\n");
    for (int i = 0; i < n; i++) {
        char p[4096];
        snprintf(p, sizeof(p), "%s/%s", repo, names[i]);
        size_t len = 0;
        unsigned char *bytes = read_whole(p, &len);
        if (!bytes) {
            die("cannot read %s", p);
        }
        osp_t pkg;
        int rc = os_package_open(bytes, len, &pkg);
        if (rc != OSP_OK) {
            die("%s: %s", p, osp_strerror(rc));
        }
        uint8_t whole[SHA256_DIGEST_BYTES];
        sha256(bytes, len, whole);
        char hex[65];
        sha256_hex(whole, hex);
        fprintf(o, "\nname: %s\n", pkg.manifest.name);
        fprintf(o, "version: %s\n", pkg.manifest.version);
        fprintf(o, "summary: %s\n", pkg.manifest.summary);
        if (pkg.manifest.requires[0]) fprintf(o, "requires: %s\n", pkg.manifest.requires);
        if (pkg.manifest.provides[0]) fprintf(o, "provides: %s\n", pkg.manifest.provides);
        if (pkg.manifest.caps[0])     fprintf(o, "caps: %s\n", pkg.manifest.caps);
        if (pkg.manifest.license[0])  fprintf(o, "license: %s\n", pkg.manifest.license);
        fprintf(o, "file: %s\n", names[i]);
        fprintf(o, "bytes: %zu\n", len);
        fprintf(o, "sha256: %s\n", hex);
        free(bytes);
        free(names[i]);
    }
    fclose(o);
    if (rename(temporary_path, out_path) != 0) {
        die("cannot rename %s to %s: %s", temporary_path, out_path, strerror(errno));
    }
    if (n == 0) {
        fprintf(stderr, "os-pkg: %s has no .osp files in it - the index is "
                        "empty\n", repo);
        return 1;
    }
    printf("os-pkg: %s - %d packages\n", out_path, n);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: os-pkg build <manifest> <stage-dir> <out.osp>\n"
                "       os-pkg info    <pkg.osp>\n"
                "       os-pkg verify  <pkg.osp>\n"
                "       os-pkg extract <pkg.osp> <dir>\n"
                "       os-pkg index   <repo-dir>\n");
        return 2;
    }
    if (strcmp(argv[1], "build") == 0 && argc == 5) {
        return command_build(argv[2], argv[3], argv[4]);
    }
    if (strcmp(argv[1], "info") == 0 && argc == 3) {
        return command_info(argv[2]);
    }
    if (strcmp(argv[1], "verify") == 0 && argc == 3) {
        return command_verify(argv[2]);
    }
    if (strcmp(argv[1], "extract") == 0 && argc == 4) {
        return command_extract(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "index") == 0 && argc == 3) {
        return command_index(argv[2]);
    }
    fprintf(stderr, "os-pkg: unknown command or wrong argument count\n");
    return 2;
}
