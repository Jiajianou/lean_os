/* user_space/bin/os.c - M111: `os`, the package manager.
 *
 *   os install <name>     install a package and what it needs
 *   os remove  <name>     take one out again
 *   os list               what is installed
 *   os available          what the repository has
 *   os info    <name>     one package, in detail
 *   os verify  [name]     re-hash installed files against what was installed
 *   os help
 *
 * ---- what "securely, in an isolated fashion" is made of ---------------
 *
 * Five things, and each of them is a property somebody can check rather
 * than a word in a sentence:
 *
 *   1. **Nothing runs at install time.** The format has no hooks (see
 *      ospkg.h). Installing is: verify, then copy. That is the whole
 *      threat surface of `os install`, and it is why the answer to "what
 *      can this package do to my machine while installing" is "write the
 *      files it lists, under a directory named after itself".
 *
 *   2. **Every byte is hashed twice on the way in.** The repository
 *      index carries the archive's SHA-256, so a bad archive is refused
 *      before it is opened; the archive's header carries a SHA-256 over
 *      its own body, and every file record carries one over its
 *      content. `os verify` re-reads the installed files months later
 *      and compares them against the third.
 *
 *   3. **A package cannot escape its prefix.** Every path in an archive
 *      is checked against ospkg_check_path (no absolute, no "..", no
 *      empty component) and then checked AGAIN after being joined onto
 *      the install root, so a rule that passed the first check and a
 *      join that produced something outside cannot both be wrong
 *      silently.
 *
 *   4. **A package cannot shadow a shipped program.** Its commands go in
 *      /pkg/bin, never in /bin. `grep` from a package is a different
 *      name in a different directory from anything this OS ships, and
 *      which one a person gets is decided by their PATH rather than by
 *      whoever installed last.
 *
 *   5. **The kernel decides what it may do, from the manifest.** A
 *      package declares its capabilities; `os` records them in
 *      /pkg/db/caps; the kernel reads that at spawn and intersects it
 *      with CAP_PKG_MAX. A package that declares nothing gets nothing -
 *      not the default, nothing - and a program under /pkg that `os` did
 *      not install gets nothing either. This is also why /bin/os holds
 *      CAP_PKG_ADMIN and is the only thing on the machine that does:
 *      every write under /pkg is refused to everything else, in the
 *      kernel, so the registry is not a file another program can edit.
 *
 * ---- and what it is not ----------------------------------------------
 *
 * There are no signatures. There is nothing to verify one against - no
 * key distribution, no trust root, one principal and no login - so a
 * signature check here would verify a key that shipped in the same image
 * as the thing it signs, which is a check that cannot fail and therefore
 * is not one. The hashes above establish INTEGRITY (these are the bytes
 * that were built) and say nothing about AUTHENTICITY (they were built
 * by someone you trust). docs/packages.md states this in the same words,
 * and names the condition under which signing becomes real work.
 *
 * Nor is any of it a defence against this disk being edited by something
 * that is not this OS. CAP_PKG_ADMIN is a rule this kernel enforces
 * while it is running.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "caps.h"    /* system_api/include/caps.h - CAP_PKG_MAX and the names */
#include "ospkg.h"
#include "sha256.h"
#include "syscall_wrappers.h"

#define PKG_ROOT_DIR   "/pkg"
#define PKG_REPO_DIR   "/pkg/repo"
#define PKG_INDEX      "/pkg/repo/index"
#define PKG_DB_DIR     "/pkg/db"
#define PKG_DB_INST    "/pkg/db/installed"
#define PKG_DB_CAPS    "/pkg/db/caps"
#define PKG_BIN_DIR    "/pkg/bin"

#define PATHBUF 512

/* The largest archive this will read into memory at once. Deliberately
 * a number rather than "whatever malloc gives me": this machine boots
 * with 128 MiB in one of its supported configurations, and a package
 * manager that cannot say in advance how much memory an install costs
 * is a package manager that fails on the small machine and not the
 * large one. 64 MiB, against a grep that is 1.3. */
#define PKG_MAX_ARCHIVE (64u * 1024u * 1024u)

/* Depth of the dependency resolution. A package that needs a package
 * that needs a package is fine; sixteen deep is a cycle or a mistake,
 * and either way stopping with a message beats recursing. */
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

/* ---- small filesystem helpers ---------------------------------------- */

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* mkdir -p. SYS_mkdir is deliberately one level (see its note in
 * syscall.h), so the recursion is here, where a package's "share/man/man1"
 * needs it. */
static int mkdir_p(const char *path) {
    char buf[PATHBUF];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') {
            continue;
        }
        buf[i] = '\0';
        if (!is_dir(buf) && mkdir(buf, 0755) != 0 && !is_dir(buf)) {
            return -1;
        }
        buf[i] = '/';
    }
    if (!is_dir(buf) && mkdir(buf, 0755) != 0 && !is_dir(buf)) {
        return -1;
    }
    return 0;
}

static unsigned char *read_whole(const char *path, size_t *len_out, size_t cap) {
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
    unsigned char *buf = malloc(want + 1);
    if (!buf) {
        close(fd);
        fprintf(stderr, "os: out of memory reading %s (%lu bytes)\n",
                path, (unsigned long)want);
        return NULL;
    }
    size_t got = 0;
    while (got < want) {
        long r = read(fd, buf + got, want - got);
        if (r <= 0) {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    if (got != want) {
        free(buf);
        return NULL;
    }
    buf[got] = '\0';
    *len_out = got;
    return buf;
}

static int write_whole(const char *path, const void *data, size_t len, int exec) {
    (void)exec; /* this filesystem has no execute bit - see docs/packages.md */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        return -1;
    }
    size_t done = 0;
    const unsigned char *p = data;
    while (done < len) {
        long w = write(fd, p + done, len - done);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        done += (size_t)w;
    }
    close(fd);
    return 0;
}

/* Remove a tree. Used by `os remove` and by the rollback in install. */
static int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return 0; /* already gone */
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
    /* Collected first, then removed: deleting entries while reading the
     * directory they are in is the kind of thing that works until the
     * directory spans two blocks.
     *
     * On the STACK and not `static`, which is the whole point. The first
     * version of this used a static array, because 128 KiB is a lot to
     * put on a user stack - and this function recurses, so the recursive
     * call would have overwritten the list its caller was still walking.
     * A package one directory deep would have removed correctly and one
     * two deep would have left files behind, silently. The answer is a
     * smaller list per level rather than a shared big one: 64 names of
     * 128 bytes is 8 KiB per frame, and a directory inside a package
     * with more than 64 entries in it is refused loudly below rather
     * than half-removed. */
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

/* ---- the repository index --------------------------------------------
 *
 * Stanzas of "key: value" separated by blank lines, which is the same
 * grammar a manifest uses - so this hands each stanza to
 * ospkg_parse_manifest and has one parser rather than two. The extra
 * keys (file, bytes, sha256) are read here because they are about the
 * archive rather than about the package.
 */
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

static void read_key(const char *text, size_t len, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    size_t i = 0;
    out[0] = '\0';
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < len) {
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
    size_t len = 0;
    unsigned char *text = read_whole(PKG_INDEX, &len, 1024u * 1024u);
    if (!text) {
        return 0;
    }
    size_t i = 0;
    while (i < len && index_count < MAX_INDEX) {
        /* One stanza: up to the first blank line. */
        size_t start = i;
        size_t end = i;
        while (i < len) {
            size_t ls = i;
            while (i < len && text[i] != '\n') {
                i++;
            }
            size_t le = i;
            if (i < len) {
                i++;
            }
            if (le == ls) { /* blank line ends the stanza */
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
        if (ospkg_parse_manifest(stanza, slen, &e->man) != OSP_OK) {
            continue; /* a comment block, or a stanza with no name */
        }
        read_key(stanza, slen, "file", e->file, sizeof(e->file));
        read_key(stanza, slen, "sha256", e->sha256_hex, sizeof(e->sha256_hex));
        char b[32];
        read_key(stanza, slen, "bytes", b, sizeof(b));
        e->bytes = strtoul(b, NULL, 10);
        if (!e->file[0] || ospkg_check_path(e->file) != OSP_OK) {
            continue; /* an index entry naming a path is not a package */
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

/* ---- what is installed ------------------------------------------------
 *
 * /pkg/db/installed/<name> holds the manifest of the installed version,
 * verbatim - so "what is installed" and "what did it say about itself"
 * are one file and cannot disagree.
 */
static int installed_version(const char *name, char *out, size_t cap) {
    char path[PATHBUF];
    snprintf(path, sizeof(path), "%s/%s", PKG_DB_INST, name);
    size_t len = 0;
    unsigned char *text = read_whole(path, &len, 64u * 1024u);
    if (!text) {
        return 0;
    }
    osp_manifest_t man;
    int rc = ospkg_parse_manifest((const char *)text, len, &man);
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
    size_t len = 0;
    unsigned char *text = read_whole(path, &len, 64u * 1024u);
    if (!text) {
        return 0;
    }
    int rc = ospkg_parse_manifest((const char *)text, len, out);
    free(text);
    return rc == OSP_OK;
}

/* ---- the capability registry the kernel reads -------------------------
 *
 * Rewritten whole from what is installed, every time anything changes,
 * rather than appended to. An append-only registry drifts from the
 * truth the first time a remove fails halfway; regenerating it means the
 * registry is a FUNCTION of /pkg/db/installed rather than a second
 * record that has to agree with it.
 *
 * Every executable an installed package provides gets a line, and so
 * does its /pkg/bin alias - because the kernel looks the spawned path up
 * literally, and /pkg/bin/grep and /pkg/grep/3.11/bin/grep are two
 * spellings a person can type.
 */
static int registry_overflowed;

static void registry_add_line(char *buf, size_t cap, size_t *len,
                              uint32_t caps, const char *path) {
    char line[PATHBUF + 32];
    int n = snprintf(line, sizeof(line), "%x %s\n", (unsigned)caps, path);
    if (n < 0 || *len + (size_t)n >= cap) {
        /* Recorded rather than dropped quietly. A registry missing a
         * line is a program that will be launched with nothing when its
         * manifest asked for something, and the symptom of that is a
         * package failing for a reason nowhere near the cause. */
        registry_overflowed = 1;
        return;
    }
    memcpy(buf + *len, line, (size_t)n);
    *len += (size_t)n;
}

static int rewrite_registry(void) {
    static char buf[24 * 1024];
    size_t len = 0;
    registry_overflowed = 0;
    const char *head =
        "# /pkg/db/caps - what each installed program may do.\n"
        "# Written by /bin/os; read by the kernel at spawn (kernel/proc/pkgcaps.c).\n"
        "# <hex capability mask> <absolute path>. Masks are intersected with\n"
        "# CAP_PKG_MAX in the kernel, so a line here cannot grant more than a\n"
        "# package is ever allowed - see system_api/include/caps.h.\n";
    len = strlen(head);
    memcpy(buf, head, len);

    DIR *d = opendir(PKG_DB_INST);
    if (!d) {
        /* Nothing installed. The registry still gets written, empty:
         * "no packages" and "the registry is missing" should not look
         * the same to whoever is debugging this. */
        return write_whole(PKG_DB_CAPS, buf, len, 0);
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
        uint32_t caps = ospkg_caps_from_names(man.caps, &unknown) & (uint32_t)CAP_PKG_MAX;
        /* Every command the package provides, under both names. */
        const char *p = man.provides;
        while (*p) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (!*p) {
                break;
            }
            char cmd[OSP_MAX_NAME];
            size_t c = 0;
            while (*p && *p != ' ' && *p != '\t' && c + 1 < sizeof(cmd)) {
                cmd[c++] = *p++;
            }
            cmd[c] = '\0';
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            char real[PATHBUF], alias[PATHBUF];
            snprintf(real, sizeof(real), "%s/%s/%s/bin/%s",
                     PKG_ROOT_DIR, man.name, man.version, cmd);
            snprintf(alias, sizeof(alias), "%s/%s", PKG_BIN_DIR, cmd);
            registry_add_line(buf, sizeof(buf), &len, caps, real);
            registry_add_line(buf, sizeof(buf), &len, caps, alias);
        }
    }
    if (registry_overflowed) {
        fprintf(stderr, "os: too many installed commands to fit in %s - "
                        "nothing has been written, because a registry with "
                        "some of the lines in it grants some packages "
                        "nothing for no visible reason\n", PKG_DB_CAPS);
        return -1;
    }
    return write_whole(PKG_DB_CAPS, buf, len, 0);
}

/* ---- install ----------------------------------------------------------- */

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
        /* Two different failures, said differently. "There is no
         * repository" and "the repository does not have that" send a
         * person to different places, and the first version of this
         * printed the second message for both - which is how a
         * truncated index on the disk presented as `os install grep`
         * reporting that grep does not exist. */
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
    snprintf(archive, sizeof(archive), "%s/%s", PKG_REPO_DIR, e->file);
    size_t len = 0;
    unsigned char *bytes = read_whole(archive, &len, PKG_MAX_ARCHIVE);
    if (!bytes) {
        fprintf(stderr, "os: cannot read %s\n", archive);
        return 1;
    }

    /* The first of the two hash checks: the archive against what the
     * index says it should be. This one is cheap to state - the index is
     * small - and it is the one that catches a repository whose archive
     * was replaced. */
    if (e->sha256_hex[0]) {
        uint8_t want[SHA256_DIGEST_BYTES], got[SHA256_DIGEST_BYTES];
        if (sha256_unhex(e->sha256_hex, want) != 0) {
            fprintf(stderr, "os: the index's sha256 for %s is not a digest\n", name);
            free(bytes);
            return 1;
        }
        sha256(bytes, len, got);
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

    /* The second: the archive against itself, plus every path rule and
     * every per-file hash. Nothing has been created yet. */
    osp_t pkg;
    int rc = ospkg_open(bytes, len, &pkg);
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
    uint32_t caps = ospkg_caps_from_names(pkg.manifest.caps, &unknown);
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
        ospkg_caps_to_names(caps & ~(uint32_t)CAP_PKG_MAX, names, sizeof(names));
        fprintf(stderr, "os: %s asks for '%s', which no package on this "
                        "machine may hold. See CAP_PKG_MAX in "
                        "system_api/include/caps.h.\n", name, names);
        free(bytes);
        return 1;
    }

    if (install_requirements(&pkg.manifest, depth) != 0) {
        free(bytes);
        return 1;
    }

    char root[PATHBUF];
    snprintf(root, sizeof(root), "%s/%s/%s", PKG_ROOT_DIR,
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
        /* The second path check, on the joined result. ospkg_open already
         * refused anything with a ".." in it; this asks the different
         * question of whether what came OUT of the join is still under
         * the root. Two checks because they can fail independently, and
         * because a directory traversal that gets through one of them is
         * the whole of the bug. */
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

        const uint8_t *data = ospkg_file_data(&pkg, i);
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
        /* Everything or nothing. A half-installed package whose files
         * are on the disk and whose database record is not is the state
         * that makes every later operation ambiguous, so the tree goes
         * away and the machine is where it was. */
        remove_tree(root);
        free(bytes);
        return 1;
    }

    /* The commands, in /pkg/bin and never in /bin. */
    if (pkg.manifest.provides[0]) {
        mkdir_p(PKG_BIN_DIR);
        const char *p = pkg.manifest.provides;
        while (*p) {
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (!*p) {
                break;
            }
            char cmd[OSP_MAX_NAME];
            size_t c = 0;
            while (*p && *p != ' ' && *p != '\t' && c + 1 < sizeof(cmd)) {
                cmd[c++] = *p++;
            }
            cmd[c] = '\0';
            while (*p && *p != ' ' && *p != '\t') {
                p++;
            }
            char real[PATHBUF], alias[PATHBUF];
            snprintf(real, sizeof(real), "%s/bin/%s", root, cmd);
            snprintf(alias, sizeof(alias), "%s/%s", PKG_BIN_DIR, cmd);
            if (!exists(real)) {
                fprintf(stderr, "os: %s says it provides '%s' and has no "
                                "bin/%s - the link is not made\n",
                        name, cmd, cmd);
                continue;
            }
            unlink(alias);
            if (symlink(real, alias) != 0) {
                fprintf(stderr, "os: cannot link %s\n", alias);
            }
        }
    }

    /* The database record last, because it is what makes the package
     * installed. Everything before this point is files on a disk that
     * nothing refers to; after it, `os list` and the kernel's registry
     * both see it. */
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
        /* The files and the record are on the disk and the kernel's view
         * of what this package may do is not. Rather than leave that,
         * the install is undone - a package the kernel would launch with
         * nothing when its manifest asked for the network is a package
         * that fails somewhere else entirely. */
        fprintf(stderr, "os: the capability registry could not be written - "
                        "%s is being removed again\n", pkg.manifest.name);
        remove_tree(root);
        unlink(rec);
        free(bytes);
        return 1;
    }

    char capnames[OSP_MAX_TEXT];
    ospkg_caps_to_names(caps, capnames, sizeof(capnames));
    say("os: installed %s-%s (%u files) in %s\n",
        pkg.manifest.name, pkg.manifest.version, pkg.file_count, root);
    if (pkg.manifest.provides[0]) {
        say("os: %s is now on this machine as %s/<command>\n",
            pkg.manifest.provides, PKG_BIN_DIR);
    }
    say("os: it may: %s\n", caps ? capnames : "nothing but read files and use the "
                                   "descriptors it is given");
    free(bytes);
    return 0;
}

/* ---- remove ------------------------------------------------------------ */

static int cmd_remove(const char *name) {
    osp_manifest_t man;
    if (!read_installed(name, &man)) {
        fprintf(stderr, "os: %s is not installed\n", name);
        return 1;
    }
    /* Anything that needs it? Asked before anything is deleted. */
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

    /* The links first, then the tree, then the record. The reverse of
     * the install order, so that a failure part way through never leaves
     * a record pointing at files that are gone. */
    const char *p = man.provides;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        char cmd[OSP_MAX_NAME];
        size_t c = 0;
        while (*p && *p != ' ' && *p != '\t' && c + 1 < sizeof(cmd)) {
            cmd[c++] = *p++;
        }
        cmd[c] = '\0';
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
        char alias[PATHBUF];
        snprintf(alias, sizeof(alias), "%s/%s", PKG_BIN_DIR, cmd);
        unlink(alias);
    }

    char root[PATHBUF];
    snprintf(root, sizeof(root), "%s/%s", PKG_ROOT_DIR, name);
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

/* ---- list, available, info, verify ------------------------------------- */

static int cmd_list(void) {
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

static int cmd_available(void) {
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

static int cmd_info(const char *name) {
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
        printf("%s/%s/%s\n", PKG_ROOT_DIR, m->name, m->version);
    }
    if (e && !inst) {
        printf("archive:   %s/%s (%lu bytes)\n", PKG_REPO_DIR, e->file, e->bytes);
    }
    return 0;
}

/* Re-hash what is on the disk against the archive it came from. The one
 * command here that can tell you something you did not already know:
 * everything else reports what a record says, and this one checks it. */
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
    snprintf(archive, sizeof(archive), "%s/%s", PKG_REPO_DIR, e->file);
    size_t len = 0;
    unsigned char *bytes = read_whole(archive, &len, PKG_MAX_ARCHIVE);
    if (!bytes) {
        fprintf(stderr, "os: cannot read %s\n", archive);
        return 1;
    }
    osp_t pkg;
    int rc = ospkg_open(bytes, len, &pkg);
    if (rc != OSP_OK) {
        fprintf(stderr, "os: %s: %s\n", archive, osp_strerror(rc));
        free(bytes);
        return 1;
    }

    char root[PATHBUF];
    snprintf(root, sizeof(root), "%s/%s/%s", PKG_ROOT_DIR, man.name, man.version);
    int bad = 0, missing = 0, checked = 0;
    for (uint32_t i = 0; i < pkg.file_count; i++) {
        const osp_file_t *f = &pkg.files[i];
        if (f->flags & OSP_F_SYMLINK) {
            continue; /* a link's content is its target; leanfs holds it */
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

static int cmd_verify(const char *name) {
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

static void usage(void) {
    printf("os - the lean_os package manager\n\n");
    printf("  os install <name>    install a package and whatever it needs\n");
    printf("  os remove  <name>    take one out again\n");
    printf("  os list              what is installed\n");
    printf("  os available         what the repository has\n");
    printf("  os info    <name>    one package, in detail\n");
    printf("  os verify  [name]    re-hash installed files against the archive\n");
    printf("  os caps              what this process may do, and what a package may\n\n");
    printf("Packages install under /pkg/<name>/<version> and their commands\n");
    printf("appear in /pkg/bin - never in /bin, so a package cannot take over\n");
    printf("the name of a program this OS ships. Nothing runs at install time.\n");
    printf("See docs/packages.md.\n");
}

/* `os caps` - what this process holds and what a package could ever
 * hold. Here rather than in /bin/caps because the interesting half is
 * the ceiling, and the ceiling is this milestone's. */
static int cmd_caps(void) {
    uint32_t mine = (uint32_t)sys_getcaps();
    char buf[OSP_MAX_TEXT];
    ospkg_caps_to_names(mine, buf, sizeof(buf));
    printf("this process:      %s\n", buf[0] ? buf : "(none)");
    ospkg_caps_to_names((uint32_t)CAP_PKG_MAX, buf, sizeof(buf));
    printf("a package may ask: %s\n", buf);
    ospkg_caps_to_names((uint32_t)CAP_ALL & ~(uint32_t)CAP_PKG_MAX, buf, sizeof(buf));
    printf("and never:         %s\n", buf);
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
    const char *cmd = argv[1];
    if (strcmp(cmd, "install") == 0 && argc >= 3) {
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
    if (strcmp(cmd, "remove") == 0 && argc >= 3) {
        return cmd_remove(argv[2]);
    }
    if (strcmp(cmd, "list") == 0) {
        return cmd_list();
    }
    if (strcmp(cmd, "available") == 0 || strcmp(cmd, "search") == 0) {
        return cmd_available();
    }
    if (strcmp(cmd, "info") == 0 && argc >= 3) {
        return cmd_info(argv[2]);
    }
    if (strcmp(cmd, "verify") == 0) {
        return cmd_verify(argc >= 3 && argv[2][0] != '-' ? argv[2] : NULL);
    }
    if (strcmp(cmd, "caps") == 0) {
        return cmd_caps();
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "--help") == 0) {
        usage();
        return 0;
    }
    fprintf(stderr, "os: '%s' is not a command. `os help` lists them.\n", cmd);
    return 2;
}
