#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "capabilities.h"
#include "os_package.h"
#include "sha256.h"
#include "syscall_wrappers.h"

#define PKG_ROOT_DIRECTORY   "/pkg"
#define PKG_REPO_DIRECTORY   "/pkg/repo"
#define PKG_INDEX      "/pkg/repo/index"
#define PKG_DB_DIRECTORY     "/pkg/db"
#define PKG_DB_INST    "/pkg/db/installed"
#define PKG_DB_CAPS    "/pkg/db/caps"
#define PKG_BIN_DIRECTORY    "/pkg/bin"

#define PATHBUF 512

#define PKG_MAX_ARCHIVE (64u * 1024u * 1024u)

#define MAX_DEPTH 16

static int quiet;

static void say(const char *fmt, ...) {
    if (quiet) {
        return;
    }
    va_list ap;
    __builtin_va_start(ap, fmt);
    vprintf(fmt, ap);
    __builtin_va_end(ap);
}

static int is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int mkdir_p(const char *path) {
    char buffer[PATHBUF];
    size_t n = strlen(path);
    if (n >= sizeof(buffer)) {
        return -1;
    }
    memcpy(buffer, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buffer[i] != '/') {
            continue;
        }
        buffer[i] = '\0';
        if (!is_directory(buffer) && mkdir(buffer, 0755) != 0 && !is_directory(buffer)) {
            return -1;
        }
        buffer[i] = '/';
    }
    if (!is_directory(buffer) && mkdir(buffer, 0755) != 0 && !is_directory(buffer)) {
        return -1;
    }
    return 0;
}

static unsigned char *read_whole(const char *path, size_t *length_out, size_t cap) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return NULL;
    }
    if ((size_t)st.st_size > cap) {
        fprintf(stderr, "os: %s is %lu bytes, over the %lu this will read\n",
                path, (unsigned long)st.st_size, (unsigned long)cap);
        return NULL;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    size_t want = (size_t)st.st_size;
    unsigned char *buffer = malloc(want + 1);
    if (!buffer) {
        close(fd);
        fprintf(stderr, "os: out of memory reading %s (%lu bytes)\n",
                path, (unsigned long)want);
        return NULL;
    }
    size_t got = 0;
    while (got < want) {
        long r = read(fd, buffer + got, want - got);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    if (got != want) {
        free(buffer);
        return NULL;
    }
    buffer[got] = '\0';
    *length_out = got;
    return buffer;
}

static int write_whole(const char *path, const void *data, size_t length, int exec) {
    (void)exec;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        return -1;
    }
    size_t done = 0;
    const unsigned char *p = data;
    while (done < length) {
        long w = write(fd, p + done, length - done);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        done += (size_t)w;
    }
    close(fd);
    return 0;
}

static int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }
    DIR *d = opendir(path);
    if (!d) {
        return -1;
    }
    struct dirent *de;
    char child[PATHBUF];
    char names[64][128];
    int n = 0;
    int overflow = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (n >= 64) {
            overflow = 1;
            break;
        }
        snprintf(names[n], sizeof(names[0]), "%s", de->d_name);
        n++;
    }
    closedir(d);
    if (overflow) {
        fprintf(stderr, "os: %s has more than 64 entries - this cannot remove "
                        "it in one pass, and a partial removal is worse than "
                        "a refusal\n", path);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        snprintf(child, sizeof(child), "%s/%s", path, names[i]);
        if (remove_tree(child) != 0) {
            return -1;
        }
    }
    return rmdir(path);
}

typedef struct {
    osp_manifest_t man;
    char           file[OSP_MAX_PATH];
    char           sha256_hex[65];
    unsigned long  bytes;
} index_entry_t;

#define MAX_INDEX 64
static index_entry_t index_entries[MAX_INDEX];
static int index_count;
static int index_loaded;

static void read_key(const char *text, size_t length, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    size_t i = 0;
    out[0] = '\0';
    while (i < length) {
        size_t start = i;
        while (i < length && text[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < length) {
            i++;
        }
        if (end - start > klen + 1 && strncmp(text + start, key, klen) == 0 &&
            text[start + klen] == ':') {
            size_t v = start + klen + 1;
            while (v < end && (text[v] == ' ' || text[v] == '\t')) {
                v++;
            }
            size_t n = end - v;
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, text + v, n);
            out[n] = '\0';
            return;
        }
    }
}

static int load_index(void) {
    if (index_loaded) {
        return index_count;
    }
    index_loaded = 1;
    size_t length = 0;
    unsigned char *text = read_whole(PKG_INDEX, &length, 1024u * 1024u);
    if (!text) {
        return 0;
    }
    size_t i = 0;
    while (i < length && index_count < MAX_INDEX) {
        size_t start = i;
        size_t end = i;
        while (i < length) {
            size_t ls = i;
            while (i < length && text[i] != '\n') {
                i++;
            }
            size_t le = i;
            if (i < length) {
                i++;
            }
            if (le == ls) {
                break;
            }
            end = i;
        }
        if (end <= start) {
            continue;
        }
        const char *stanza = (const char *)text + start;
        size_t slen = end - start;
        index_entry_t *e = &index_entries[index_count];
        memset(e, 0, sizeof(*e));
        if (os_package_parse_manifest(stanza, slen, &e->man) != OSP_OK) {
            continue;
        }
        read_key(stanza, slen, "file", e->file, sizeof(e->file));
        read_key(stanza, slen, "sha256", e->sha256_hex, sizeof(e->sha256_hex));
        char b[32];
        read_key(stanza, slen, "bytes", b, sizeof(b));
        e->bytes = strtoul(b, NULL, 10);
        if (!e->file[0] || os_package_check_path(e->file) != OSP_OK) {
            continue;
        }
        index_count++;
    }
    free(text);
    return index_count;
}

static index_entry_t *find_in_index(const char *name) {
    load_index();
    for (int i = 0; i < index_count; i++) {
        if (strcmp(index_entries[i].man.name, name) == 0) {
            return &index_entries[i];
        }
    }
    return NULL;
}

static int installed_version(const char *name, char *out, size_t cap) {
    char path[PATHBUF];
    snprintf(path, sizeof(path), "%s/%s", PKG_DB_INST, name);
    size_t length = 0;
    unsigned char *text = read_whole(path, &length, 64u * 1024u);
    if (!text) {
        return 0;
    }
    osp_manifest_t man;
    int rc = os_package_parse_manifest((const char *)text, length, &man);
    free(text);
    if (rc != OSP_OK) {
        return 0;
    }
    snprintf(out, cap, "%s", man.version);
    return 1;
}

static int read_installed(const char *name, osp_manifest_t *out) {
    char path[PATHBUF];
    snprintf(path, sizeof(path), "%s/%s", PKG_DB_INST, name);
    size_t length = 0;
    unsigned char *text = read_whole(path, &length, 64u * 1024u);
    if (!text) {
        return 0;
    }
    int rc = os_package_parse_manifest((const char *)text, length, out);
    free(text);
    return rc == OSP_OK;
}

static int registry_overflowed;

static void registry_add_line(char *buffer, size_t cap, size_t *length,
                              uint32_t caps, const char *path) {
    char line[PATHBUF + 32];
    int n = snprintf(line, sizeof(line), "%x %s\n", (unsigned)caps, path);
    if (n < 0 || *length + (size_t)n >= cap) {
        registry_overflowed = 1;
        return;
    }
    memcpy(buffer + *length, line, (size_t)n);
    *length += (size_t)n;
}

static int rewrite_registry(void) {
    static char buffer[24 * 1024];
    size_t length = 0;
    registry_overflowed = 0;
    const char *head =
        "# /pkg/db/caps - what each installed program may do.\n"
        "# Written by /bin/os; read by the kernel at spawn (kernel/process/package_capabilities.c).\n"
        "# <hex capability mask> <absolute path>. Masks are intersected with\n"
        "# CAP_PKG_MAX in the kernel, so a line here cannot grant more than a\n"
        "# package is ever allowed - see system_api/include/capabilities.h.\n";
    length = strlen(head);
    memcpy(buffer, head, length);

    DIR *d = opendir(PKG_DB_INST);
    if (!d) {
        return write_whole(PKG_DB_CAPS, buffer, length, 0);
    }
    struct dirent *de;
    static char names[MAX_INDEX][OSP_MAX_NAME];
    int n = 0;
    while ((de = readdir(d)) != NULL && n < MAX_INDEX) {
        if (de->d_name[0] == '.') {
            continue;
        }
        snprintf(names[n], sizeof(names[0]), "%s", de->d_name);
        n++;
    }
    closedir(d);

    for (int i = 0; i < n; i++) {
        osp_manifest_t man;
        if (!read_installed(names[i], &man)) {
            continue;
        }
        int unknown = 0;
        uint32_t caps = os_package_caps_from_names(man.caps, &unknown) & (uint32_t)CAP_PKG_MAX;
        const char *p = man.provides;
        while (*p) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (!*p) {
                break;
            }
            char command[OSP_MAX_NAME];
            size_t c = 0;
            while (*p && *p != ' ' && *p != '\t' && c + 1 < sizeof(command)) {
                command[c++] = *p++;
            }
            command[c] = '\0';
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            char real[PATHBUF], alias[PATHBUF];
            snprintf(real, sizeof(real), "%s/%s/%s/bin/%s",
                     PKG_ROOT_DIRECTORY, man.name, man.version, command);
            snprintf(alias, sizeof(alias), "%s/%s", PKG_BIN_DIRECTORY, command);
            registry_add_line(buffer, sizeof(buffer), &length, caps, real);
            registry_add_line(buffer, sizeof(buffer), &length, caps, alias);
        }
    }
    if (registry_overflowed) {
        fprintf(stderr, "os: too many installed commands to fit in %s - "
                        "nothing has been written, because a registry with "
                        "some of the lines in it grants some packages "
                        "nothing for no visible reason\n", PKG_DB_CAPS);
        return -1;
    }
    return write_whole(PKG_DB_CAPS, buffer, length, 0);
}

static int install_one(const char *name, int depth);

static int install_requirements(const osp_manifest_t *man, int depth) {
    const char *p = man->requires;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        char dep[OSP_MAX_NAME];
        size_t c = 0;
        while (*p && *p != ' ' && *p != '\t' && c + 1 < sizeof(dep)) {
            dep[c++] = *p++;
        }
        dep[c] = '\0';
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        char have[OSP_MAX_VERSION];
        if (installed_version(dep, have, sizeof(have))) {
            continue;
        }
        say("os: %s needs %s\n", man->name, dep);
        if (install_one(dep, depth + 1) != 0) {
            return -1;
        }
    }
    return 0;
}

static int install_one(const char *name, int depth) {
    if (depth > MAX_DEPTH) {
        fprintf(stderr, "os: dependencies nested more than %d deep - this is "
                        "a cycle in the repository, not a package\n", MAX_DEPTH);
        return 1;
    }

    index_entry_t *e = find_in_index(name);
    if (!e) {
        if (index_count == 0) {
            fprintf(stderr, "os: this machine's package index (%s) lists no "
                            "packages at all. It is missing or unreadable - "
                            "run tools/build-packages.sh and `make packages` "
                            "on the machine that built this image.\n",
                    PKG_INDEX);
        } else {
            fprintf(stderr, "os: no package called '%s'. `os available` lists "
                            "the %d this machine's repository has.\n",
                    name, index_count);
        }
        return 1;
    }

    char have[OSP_MAX_VERSION];
    if (installed_version(name, have, sizeof(have))) {
        if (strcmp(have, e->man.version) == 0) {
            say("os: %s-%s is already installed\n", name, have);
            return 0;
        }
        fprintf(stderr, "os: %s-%s is installed and the repository has %s. "
                        "Remove it first - this version does not upgrade in "
                        "place, and pretending to would leave two versions' "
                        "files in one directory.\n",
                name, have, e->man.version);
        return 1;
    }

    char archive[PATHBUF];
    snprintf(archive, sizeof(archive), "%s/%s", PKG_REPO_DIRECTORY, e->file);
    size_t length = 0;
    unsigned char *bytes = read_whole(archive, &length, PKG_MAX_ARCHIVE);
    if (!bytes) {
        fprintf(stderr, "os: cannot read %s\n", archive);
        return 1;
    }

    if (e->sha256_hex[0]) {
        uint8_t want[SHA256_DIGEST_BYTES], got[SHA256_DIGEST_BYTES];
        if (sha256_unhex(e->sha256_hex, want) != 0) {
            fprintf(stderr, "os: the index's sha256 for %s is not a digest\n", name);
            free(bytes);
            return 1;
        }
        sha256(bytes, length, got);
        if (!sha256_equal(want, got)) {
            char hex[65];
            sha256_hex(got, hex);
            fprintf(stderr, "os: %s does not match the index.\n", archive);
            fprintf(stderr, "    index says %s\n", e->sha256_hex);
            fprintf(stderr, "    the file is %s\n", hex);
            free(bytes);
            return 1;
        }
    }

    osp_t pkg;
    int rc = os_package_open(bytes, length, &pkg);
    if (rc != OSP_OK) {
        fprintf(stderr, "os: %s: %s\n", archive, osp_strerror(rc));
        free(bytes);
        return 1;
    }
    if (strcmp(pkg.manifest.name, name) != 0) {
        fprintf(stderr, "os: %s calls itself '%s' - the index and the archive "
                        "disagree about what this package is\n",
                archive, pkg.manifest.name);
        free(bytes);
        return 1;
    }

    int unknown = 0;
    uint32_t caps = os_package_caps_from_names(pkg.manifest.caps, &unknown);
    if (unknown) {
        fprintf(stderr, "os: %s asks for a capability this OS does not have "
                        "('%s'). Refused rather than ignored: a name this "
                        "machine cannot enforce is not a name to accept.\n",
                name, pkg.manifest.caps);
        free(bytes);
        return 1;
    }
    if (caps & ~(uint32_t)CAP_PKG_MAX) {
        char names[OSP_MAX_TEXT];
        os_package_caps_to_names(caps & ~(uint32_t)CAP_PKG_MAX, names, sizeof(names));
        fprintf(stderr, "os: %s asks for '%s', which no package on this "
                        "machine may hold. See CAP_PKG_MAX in "
                        "system_api/include/capabilities.h.\n", name, names);
        free(bytes);
        return 1;
    }

    if (install_requirements(&pkg.manifest, depth) != 0) {
        free(bytes);
        return 1;
    }

    char root[PATHBUF];
    snprintf(root, sizeof(root), "%s/%s/%s", PKG_ROOT_DIRECTORY,
             pkg.manifest.name, pkg.manifest.version);
    if (mkdir_p(root) != 0) {
        fprintf(stderr, "os: cannot create %s\n", root);
        free(bytes);
        return 1;
    }

    int failed = 0;
    for (uint32_t i = 0; i < pkg.file_count && !failed; i++) {
        const osp_file_t *f = &pkg.files[i];
        char dest[PATHBUF];
        if ((size_t)snprintf(dest, sizeof(dest), "%s/%s", root, f->path) >= sizeof(dest)) {
            fprintf(stderr, "os: %s is too long once joined onto %s\n", f->path, root);
            failed = 1;
            break;
        }
        if (strncmp(dest, root, strlen(root)) != 0 || dest[strlen(root)] != '/') {
            fprintf(stderr, "os: %s would be installed outside %s - refused\n",
                    f->path, root);
            failed = 1;
            break;
        }

        char parent[PATHBUF];
        snprintf(parent, sizeof(parent), "%s", dest);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            if (mkdir_p(parent) != 0) {
                fprintf(stderr, "os: cannot create %s\n", parent);
                failed = 1;
                break;
            }
        }

        const uint8_t *data = os_package_file_data(&pkg, i);
        if (f->flags & OSP_F_SYMLINK) {
            char target[OSP_MAX_PATH + 1];
            memcpy(target, data, (size_t)f->size);
            target[f->size] = '\0';
            unlink(dest);
            if (symlink(target, dest) != 0) {
                fprintf(stderr, "os: cannot link %s -> %s\n", dest, target);
                failed = 1;
            }
            continue;
        }
        if (write_whole(dest, data, (size_t)f->size, (f->flags & OSP_F_EXEC) != 0) != 0) {
            fprintf(stderr, "os: cannot write %s\n", dest);
            failed = 1;
        }
    }

    if (failed) {
        remove_tree(root);
        free(bytes);
        return 1;
    }

    if (pkg.manifest.provides[0]) {
        mkdir_p(PKG_BIN_DIRECTORY);
        const char *p = pkg.manifest.provides;
        while (*p) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (!*p) {
                break;
            }
            char command[OSP_MAX_NAME];
            size_t c = 0;
            while (*p && *p != ' ' && *p != '\t' && c + 1 < sizeof(command)) {
                command[c++] = *p++;
            }
            command[c] = '\0';
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            char real[PATHBUF], alias[PATHBUF];
            snprintf(real, sizeof(real), "%s/bin/%s", root, command);
            snprintf(alias, sizeof(alias), "%s/%s", PKG_BIN_DIRECTORY, command);
            if (!exists(real)) {
                fprintf(stderr, "os: %s says it provides '%s' and has no "
                                "bin/%s - the link is not made\n",
                        name, command, command);
                continue;
            }
            unlink(alias);
            if (symlink(real, alias) != 0) {
                fprintf(stderr, "os: cannot link %s\n", alias);
            }
        }
    }

    mkdir_p(PKG_DB_INST);
    char rec[PATHBUF];
    snprintf(rec, sizeof(rec), "%s/%s", PKG_DB_INST, name);
    char text[2048];
    int tn = snprintf(text, sizeof(text),
                      "name: %s\nversion: %s\nsummary: %s\n",
                      pkg.manifest.name, pkg.manifest.version, pkg.manifest.summary);
    if (pkg.manifest.requires[0]) {
        tn += snprintf(text + tn, sizeof(text) - tn, "requires: %s\n", pkg.manifest.requires);
    }
    if (pkg.manifest.provides[0]) {
        tn += snprintf(text + tn, sizeof(text) - tn, "provides: %s\n", pkg.manifest.provides);
    }
    if (pkg.manifest.caps[0]) {
        tn += snprintf(text + tn, sizeof(text) - tn, "caps: %s\n", pkg.manifest.caps);
    }
    if (pkg.manifest.license[0]) {
        tn += snprintf(text + tn, sizeof(text) - tn, "license: %s\n", pkg.manifest.license);
    }
    if (pkg.manifest.source[0]) {
        tn += snprintf(text + tn, sizeof(text) - tn, "source: %s\n", pkg.manifest.source);
    }
    tn += snprintf(text + tn, sizeof(text) - tn, "files: %u\n", pkg.file_count);
    if (write_whole(rec, text, (size_t)tn, 0) != 0) {
        fprintf(stderr, "os: cannot write %s - the files are installed and "
                        "the record is not, so they are being removed again\n", rec);
        remove_tree(root);
        free(bytes);
        return 1;
    }

    if (rewrite_registry() != 0) {
        fprintf(stderr, "os: the capability registry could not be written - "
                        "%s is being removed again\n", pkg.manifest.name);
        remove_tree(root);
        unlink(rec);
        free(bytes);
        return 1;
    }

    char capnames[OSP_MAX_TEXT];
    os_package_caps_to_names(caps, capnames, sizeof(capnames));
    say("os: installed %s-%s (%u files) in %s\n",
        pkg.manifest.name, pkg.manifest.version, pkg.file_count, root);
    if (pkg.manifest.provides[0]) {
        say("os: %s is now on this machine as %s/<command>\n",
            pkg.manifest.provides, PKG_BIN_DIRECTORY);
    }
    say("os: it may: %s\n", caps ? capnames : "nothing but read files and use the "
                                   "descriptors it is given");
    free(bytes);
    return 0;
}

static int command_remove(const char *name) {
    osp_manifest_t man;
    if (!read_installed(name, &man)) {
        fprintf(stderr, "os: %s is not installed\n", name);
        return 1;
    }
    DIR *d = opendir(PKG_DB_INST);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.' || strcmp(de->d_name, name) == 0) {
                continue;
            }
            osp_manifest_t other;
            if (!read_installed(de->d_name, &other)) {
                continue;
            }
            const char *p = other.requires;
            while (*p) {
                while (*p == ' ' || *p == '\t') {
                    p++;
                }
                if (!*p) {
                    break;
                }
                const char *start = p;
                while (*p && *p != ' ' && *p != '\t') {
                    p++;
                }
                if ((size_t)(p - start) == strlen(name) &&
                    strncmp(start, name, (size_t)(p - start)) == 0) {
                    fprintf(stderr, "os: %s needs %s - remove that first\n",
                            other.name, name);
                    closedir(d);
                    return 1;
                }
            }
        }
        closedir(d);
    }

    const char *p = man.provides;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        char command[OSP_MAX_NAME];
        size_t c = 0;
        while (*p && *p != ' ' && *p != '\t' && c + 1 < sizeof(command)) {
            command[c++] = *p++;
        }
        command[c] = '\0';
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        char alias[PATHBUF];
        snprintf(alias, sizeof(alias), "%s/%s", PKG_BIN_DIRECTORY, command);
        unlink(alias);
    }

    char root[PATHBUF];
    snprintf(root, sizeof(root), "%s/%s", PKG_ROOT_DIRECTORY, name);
    if (remove_tree(root) != 0) {
        fprintf(stderr, "os: could not remove %s\n", root);
        return 1;
    }
    char rec[PATHBUF];
    snprintf(rec, sizeof(rec), "%s/%s", PKG_DB_INST, name);
    unlink(rec);
    rewrite_registry();
    say("os: removed %s-%s\n", name, man.version);
    return 0;
}

static int command_list(void) {
    DIR *d = opendir(PKG_DB_INST);
    if (!d) {
        printf("os: nothing is installed. `os available` lists what this "
               "machine's repository has.\n");
        return 0;
    }
    struct dirent *de;
    int n = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        osp_manifest_t man;
        if (!read_installed(de->d_name, &man)) {
            continue;
        }
        printf("%-16s %-10s %s\n", man.name, man.version, man.summary);
        n++;
    }
    closedir(d);
    if (n == 0) {
        printf("os: nothing is installed.\n");
    }
    return 0;
}

static int command_available(void) {
    if (load_index() == 0) {
        printf("os: no repository on this machine (%s).\n", PKG_INDEX);
        return 1;
    }
    for (int i = 0; i < index_count; i++) {
        char have[OSP_MAX_VERSION];
        int inst = installed_version(index_entries[i].man.name, have, sizeof(have));
        printf("%-16s %-10s %s%s\n", index_entries[i].man.name,
               index_entries[i].man.version,
               inst ? "[installed] " : "",
               index_entries[i].man.summary);
    }
    return 0;
}

static int command_info(const char *name) {
    osp_manifest_t man;
    int inst = read_installed(name, &man);
    index_entry_t *e = find_in_index(name);
    if (!inst && !e) {
        fprintf(stderr, "os: no package called '%s'\n", name);
        return 1;
    }
    const osp_manifest_t *m = inst ? &man : &e->man;
    printf("name:      %s\n", m->name);
    printf("version:   %s\n", m->version);
    printf("summary:   %s\n", m->summary);
    if (m->license[0])  printf("license:   %s\n", m->license);
    if (m->source[0])   printf("source:    %s\n", m->source);
    if (m->requires[0]) printf("requires:  %s\n", m->requires);
    if (m->provides[0]) printf("provides:  %s\n", m->provides);
    printf("caps:      %s\n", m->caps[0] ? m->caps : "(none)");
    printf("installed: %s", inst ? "yes, at " : "no\n");
    if (inst) {
        printf("%s/%s/%s\n", PKG_ROOT_DIRECTORY, m->name, m->version);
    }
    if (e && !inst) {
        printf("archive:   %s/%s (%lu bytes)\n", PKG_REPO_DIRECTORY, e->file, e->bytes);
    }
    return 0;
}

static int verify_one(const char *name) {
    osp_manifest_t man;
    if (!read_installed(name, &man)) {
        fprintf(stderr, "os: %s is not installed\n", name);
        return 1;
    }
    index_entry_t *e = find_in_index(name);
    if (!e) {
        fprintf(stderr, "os: %s is installed but the repository no longer has "
                        "it, so there is nothing to compare against\n", name);
        return 1;
    }
    char archive[PATHBUF];
    snprintf(archive, sizeof(archive), "%s/%s", PKG_REPO_DIRECTORY, e->file);
    size_t length = 0;
    unsigned char *bytes = read_whole(archive, &length, PKG_MAX_ARCHIVE);
    if (!bytes) {
        fprintf(stderr, "os: cannot read %s\n", archive);
        return 1;
    }
    osp_t pkg;
    int rc = os_package_open(bytes, length, &pkg);
    if (rc != OSP_OK) {
        fprintf(stderr, "os: %s: %s\n", archive, osp_strerror(rc));
        free(bytes);
        return 1;
    }

    char root[PATHBUF];
    snprintf(root, sizeof(root), "%s/%s/%s", PKG_ROOT_DIRECTORY, man.name, man.version);
    int bad = 0, missing = 0, checked = 0;
    for (uint32_t i = 0; i < pkg.file_count; i++) {
        const osp_file_t *f = &pkg.files[i];
        if (f->flags & OSP_F_SYMLINK) {
            continue;
        }
        char dest[PATHBUF];
        snprintf(dest, sizeof(dest), "%s/%s", root, f->path);
        size_t got = 0;
        unsigned char *data = read_whole(dest, &got, PKG_MAX_ARCHIVE);
        if (!data) {
            printf("  MISSING  %s\n", f->path);
            missing++;
            continue;
        }
        uint8_t digest[SHA256_DIGEST_BYTES];
        sha256(data, got, digest);
        free(data);
        checked++;
        if (got != f->size || !sha256_equal(digest, f->sha256)) {
            printf("  CHANGED  %s\n", f->path);
            bad++;
        }
    }
    free(bytes);
    if (bad || missing) {
        printf("os: %s-%s - %d changed, %d missing, of %u files\n",
               man.name, man.version, bad, missing, pkg.file_count);
        return 1;
    }
    printf("os: %s-%s - %d files, every one byte for byte as installed\n",
           man.name, man.version, checked);
    return 0;
}

static int command_verify(const char *name) {
    if (name) {
        return verify_one(name);
    }
    DIR *d = opendir(PKG_DB_INST);
    if (!d) {
        printf("os: nothing is installed.\n");
        return 0;
    }
    struct dirent *de;
    static char names[MAX_INDEX][OSP_MAX_NAME];
    int n = 0;
    while ((de = readdir(d)) != NULL && n < MAX_INDEX) {
        if (de->d_name[0] == '.') {
            continue;
        }
        snprintf(names[n], sizeof(names[0]), "%s", de->d_name);
        n++;
    }
    closedir(d);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        if (verify_one(names[i]) != 0) {
            rc = 1;
        }
    }
    return rc;
}

#define PKG_PREINSTALL      PKG_REPO_DIRECTORY "/preinstall"
#define PKG_DB_PREINSTALLED PKG_DB_DIRECTORY "/preinstalled"

static int list_has_line(const char *list, const char *name) {
    size_t n = strlen(name);
    const char *p = list;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t length = eol ? (size_t)(eol - p) : strlen(p);
        if (length == n && strncmp(p, name, n) == 0) {
            return 1;
        }
        p = eol ? eol + 1 : NULL;
    }
    return 0;
}

static int command_preinstall(void) {
    size_t length = 0;
    char *list = (char *)read_whole(PKG_PREINSTALL, &length, 4096);
    if (!list) {
        return 0;
    }
    size_t dlen = 0;
    char *done = (char *)read_whole(PKG_DB_PREINSTALLED, &dlen, 4096);
    char record[4096];
    size_t rlen = 0;
    if (done) {
        rlen = dlen < sizeof(record) - 1 ? dlen : sizeof(record) - 1;
        memcpy(record, done, rlen);
    }
    record[rlen] = '\0';

    int rc = 0, changed = 0;
    char *p = list;
    while (p && *p) {
        char *eol = strchr(p, '\n');
        if (eol) {
            *eol = '\0';
        }
        char *name = p;
        p = eol ? eol + 1 : NULL;
        if (!name[0] || name[0] == '#' || list_has_line(record, name)) {
            continue;
        }
        if (install_one(name, 0) != 0) {
            fprintf(stderr, "os: preinstall: %s did not install - it will be tried again next boot\n", name);
            rc = 1;
            continue;
        }
        size_t n = strlen(name);
        if (rlen + n + 1 < sizeof(record)) {
            memcpy(record + rlen, name, n);
            rlen += n;
            record[rlen++] = '\n';
            record[rlen] = '\0';
            changed = 1;
        }
    }
    if (changed && (mkdir_p(PKG_DB_DIRECTORY) != 0 ||
                    write_whole(PKG_DB_PREINSTALLED, record, rlen, 0) != 0)) {
        fprintf(stderr, "os: preinstall: could not record what was installed in %s\n",
                PKG_DB_PREINSTALLED);
        rc = 1;
    }
    free(done);
    free(list);
    return rc;
}

static void usage(void) {
    printf("os - the lean_os package manager\n\n");
    printf("  os install <name>    install a package and whatever it needs\n");
    printf("  os remove  <name>    take one out again\n");
    printf("  os list              what is installed\n");
    printf("  os available         what the repository has\n");
    printf("  os info    <name>    one package, in detail\n");
    printf("  os verify  [name]    re-hash installed files against the archive\n");
    printf("  os caps              what this process may do, and what a package may\n");
    printf("  os preinstall        install what this image lists in /pkg/repo/preinstall,\n");
    printf("                       once each (init runs it at boot)\n\n");
    printf("Packages install under /pkg/<name>/<version> and their commands\n");
    printf("appear in /pkg/bin - never in /bin, so a package cannot take over\n");
    printf("the name of a program this OS ships. Nothing runs at install time.\n");
    printf("See docs/packages.md.\n");
}

static int command_caps(void) {
    uint32_t mine = (uint32_t)sys_getcaps();
    char buffer[OSP_MAX_TEXT];
    os_package_caps_to_names(mine, buffer, sizeof(buffer));
    printf("this process:      %s\n", buffer[0] ? buffer : "(none)");
    os_package_caps_to_names((uint32_t)CAP_PKG_MAX, buffer, sizeof(buffer));
    printf("a package may ask: %s\n", buffer);
    os_package_caps_to_names((uint32_t)CAP_ALL & ~(uint32_t)CAP_PKG_MAX, buffer, sizeof(buffer));
    printf("and never:         %s\n", buffer);
    printf("an unlisted program under /pkg gets: %s\n",
           CAP_PKG_UNLISTED ? "something" : "nothing at all");
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        }
    }
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *command = argv[1];
    if (strcmp(command, "install") == 0 && argc >= 3) {
        int rc = 0;
        for (int i = 2; i < argc; i++) {
            if (argv[i][0] == '-') {
                continue;
            }
            if (install_one(argv[i], 0) != 0) {
                rc = 1;
            }
        }
        return rc;
    }
    if (strcmp(command, "remove") == 0 && argc >= 3) {
        return command_remove(argv[2]);
    }
    if (strcmp(command, "list") == 0) {
        return command_list();
    }
    if (strcmp(command, "available") == 0 || strcmp(command, "search") == 0) {
        return command_available();
    }
    if (strcmp(command, "info") == 0 && argc >= 3) {
        return command_info(argv[2]);
    }
    if (strcmp(command, "verify") == 0) {
        return command_verify(argc >= 3 && argv[2][0] != '-' ? argv[2] : NULL);
    }
    if (strcmp(command, "caps") == 0) {
        return command_caps();
    }
    if (strcmp(command, "preinstall") == 0) {
        return command_preinstall();
    }
    if (strcmp(command, "help") == 0 || strcmp(command, "-h") == 0 ||
        strcmp(command, "--help") == 0) {
        usage();
        return 0;
    }
    fprintf(stderr, "os: '%s' is not a command. `os help` lists them.\n", command);
    return 2;
}
