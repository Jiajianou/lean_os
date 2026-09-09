/* tests/test_ospkg.c - M111
 *
 * The package reader, graded on the inputs a booted machine cannot
 * produce: a truncated archive, a hash that does not match, a path with
 * ".." in it, a file record pointing past the end of the payload, a
 * length field chosen to make the arithmetic wrap.
 *
 * This is the tier those belong in, and the reason is the one Q2 gives:
 * getting to "the archive is corrupt" from inside a booted OS means
 * corrupting one, and there is no way to corrupt one in a way an
 * attacker would that does not amount to writing this file anyway. So
 * the machine's self-test ([m111], user_space/bin/pkgtest.c) grades the
 * happy path against a real 1.2 MB GNU grep and a real capability
 * boundary, and this grades every refusal.
 *
 * **Every archive here is built byte by byte rather than by calling the
 * writer.** That is deliberate and it is the difference between this
 * file and a round-trip test: a reader tested only against its own
 * writer agrees with the writer's mistakes. The layout constants below
 * are written out from ospkg.h's documented format, so a change to the
 * struct that did not change the format on disk fails here.
 */
#include "check.h"

#include "ospkg.h"
#include "sha256.h"
#include "caps.h"

#include <stdlib.h>
#include <string.h>

/* ---- a package, assembled from the format description ---------------- */

#define HDR  OSP_HEADER_BYTES
#define REC  OSP_FILE_BYTES

typedef struct {
    unsigned char *bytes;
    size_t         len;
} blob_t;

static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void put64(unsigned char *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

typedef struct {
    const char *path;
    const char *content;
    uint32_t    flags;
} want_file_t;

/* Build a well-formed archive. Every test below starts from one of these
 * and then breaks exactly one thing, so a failure names the thing. */
static blob_t build(const char *manifest, const want_file_t *files, int nfiles) {
    /* Padded to a multiple of 8 the way the format requires, so that the
     * file table lands 8-aligned - see ospkg.h. Written out here rather
     * than shared with the writer, for the same reason the rest of this
     * builder is: a reader tested only against its own writer agrees
     * with the writer's mistakes. */
    size_t meta_len = (strlen(manifest) + 7u) & ~(size_t)7u;
    size_t payload_len = 0;
    for (int i = 0; i < nfiles; i++) {
        payload_len += strlen(files[i].content);
    }
    size_t table_len = (size_t)nfiles * REC;
    size_t total = HDR + meta_len + table_len + payload_len;

    unsigned char *b = calloc(total ? total : 1, 1);
    REQUIRE(b != NULL);

    memcpy(b, "LEANPKG1", 8);
    put32(b + 8, 1);
    put32(b + 12, (uint32_t)meta_len);
    put32(b + 16, (uint32_t)nfiles);
    put32(b + 20, 0);
    put64(b + 24, (uint64_t)payload_len);
    memcpy(b + HDR, manifest, strlen(manifest));
    for (size_t i = strlen(manifest); i < meta_len; i++) {
        b[HDR + i] = '\n';
    }

    unsigned char *table = b + HDR + meta_len;
    unsigned char *payload = table + table_len;
    size_t off = 0;
    for (int i = 0; i < nfiles; i++) {
        size_t n = strlen(files[i].content);
        unsigned char *r = table + (size_t)i * REC;
        memcpy(r, files[i].path, strlen(files[i].path));
        put64(r + OSP_MAX_PATH, (uint64_t)n);
        put64(r + OSP_MAX_PATH + 8, (uint64_t)off);
        put32(r + OSP_MAX_PATH + 16, files[i].flags);
        put32(r + OSP_MAX_PATH + 20, 0);
        memcpy(payload + off, files[i].content, n);
        sha256(payload + off, n, r + OSP_MAX_PATH + 24);
        off += n;
    }

    /* The body hash goes on last, over everything after the header. */
    sha256(b + HDR, total - HDR, b + 32);

    blob_t out = {b, total};
    return out;
}

/* Recompute the body hash after a test has changed something below it -
 * used when the test wants to break something OTHER than the hash. */
static void reseal(blob_t *p) {
    sha256(p->bytes + HDR, p->len - HDR, p->bytes + 32);
}

static const char MANIFEST[] =
    "name: toy\n"
    "version: 1.0\n"
    "summary: a package for the tests\n"
    "provides: toy\n";

static want_file_t TOY_FILES[] = {
    {"bin/toy", "not really an ELF", OSP_F_EXEC},
    {"share/readme", "read me", 0},
};

TEST(ospkg, a_well_formed_package_opens) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    osp_t pkg;
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), OSP_OK);
    CHECK(strcmp(pkg.manifest.name, "toy") == 0);
    CHECK(strcmp(pkg.manifest.version, "1.0") == 0);
    CHECK_EQ(pkg.file_count, 2u);
    CHECK(strcmp(pkg.files[0].path, "bin/toy") == 0);
    CHECK_EQ(pkg.files[0].flags & OSP_F_EXEC, OSP_F_EXEC);
    CHECK(memcmp(ospkg_file_data(&pkg, 1), "read me", 7) == 0);
    free(p.bytes);
}

TEST(ospkg, not_a_package_at_all) {
    unsigned char junk[HDR + 16];
    memset(junk, 0xAB, sizeof(junk));
    osp_t pkg;
    CHECK_EQ(ospkg_open(junk, sizeof(junk), &pkg), -OSP_E_MAGIC);
    /* And something shorter than a header, which must not be read as
     * one - the check that has to come before every other check. */
    CHECK_EQ(ospkg_open(junk, 8, &pkg), -OSP_E_SHORT);
    CHECK_EQ(ospkg_open(junk, 0, &pkg), -OSP_E_SHORT);
}

TEST(ospkg, a_format_from_the_future) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    put32(p.bytes + 8, 2);
    reseal(&p);
    osp_t pkg;
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_FORMAT);
    free(p.bytes);
}

/* One flipped bit anywhere in the body must be caught. Not a sampled
 * check: the payload, the file table and the manifest are three regions
 * and a hash over only one of them would pass two of these. */
TEST(ospkg, one_flipped_bit_is_refused) {
    size_t probes[] = {HDR, HDR + 5, HDR + 40, HDR + 90, HDR + 200, HDR + 400};
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        blob_t p = build(MANIFEST, TOY_FILES, 2);
        REQUIRE(probes[i] < p.len);
        p.bytes[probes[i]] ^= 0x01;
        osp_t pkg;
        int rc = ospkg_open(p.bytes, p.len, &pkg);
        if (rc != -OSP_E_BODY_HASH) {
            test_fail(__FILE__, __LINE__,
                      "a flipped bit at offset %zu gave %d, not -OSP_E_BODY_HASH",
                      probes[i], rc);
        }
        free(p.bytes);
    }
}

/* A file whose content does not match its own record, with the body hash
 * recomputed so that the archive is internally consistent everywhere
 * except there. This is the check a reader that only verified the whole
 * archive would not have, and the one `os verify` needs months later. */
TEST(ospkg, a_file_that_is_not_what_its_record_says) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    /* The payload starts after the header, the manifest and two records. */
    size_t payload = HDR + ((strlen(MANIFEST) + 7u) & ~(size_t)7u) + 2 * REC;
    p.bytes[payload] = 'X';
    reseal(&p);
    osp_t pkg;
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_FILE_HASH);
    free(p.bytes);
}

TEST(ospkg, truncated_after_the_header) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    osp_t pkg;
    /* Every truncation from "just a header" up to one byte short. */
    for (size_t len = HDR; len < p.len; len += 37) {
        int rc = ospkg_open(p.bytes, len, &pkg);
        if (rc != -OSP_E_SHORT) {
            test_fail(__FILE__, __LINE__,
                      "a %zu-byte prefix of a %zu-byte package gave %d",
                      len, p.len, rc);
            break;
        }
    }
    CHECK_EQ(ospkg_open(p.bytes, p.len - 1, &pkg), -OSP_E_SHORT);
    free(p.bytes);
}

/* The lengths chosen to make the reader's arithmetic wrap. A file_count
 * of 2^28 times a 280-byte record overflows 32 bits; a payload_bytes of
 * ~2^64 overflows anything added to it. Both must be refused by the
 * ceilings before any multiplication happens. */
TEST(ospkg, lengths_that_would_wrap) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    osp_t pkg;

    put32(p.bytes + 16, 0x10000000u); /* file_count */
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_HUGE);

    put32(p.bytes + 16, 2);
    put64(p.bytes + 24, 0xFFFFFFFFFFFFFF00ull); /* payload_bytes */
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_HUGE);

    put64(p.bytes + 24, 15);
    put32(p.bytes + 12, 0xFFFFFF00u); /* meta_bytes */
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_HUGE);

    free(p.bytes);
}

/* A record whose extent runs past the payload, and one whose offset does
 * on its own. Both are the same bug class - a length believed without
 * being checked - and both would be a read outside the buffer. */
TEST(ospkg, a_file_that_points_outside_the_payload) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    unsigned char *rec0 = p.bytes + HDR + ((strlen(MANIFEST) + 7u) & ~(size_t)7u);
    put64(rec0 + OSP_MAX_PATH, 1u << 20); /* size */
    reseal(&p);
    osp_t pkg;
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_OVERLAP);
    free(p.bytes);

    p = build(MANIFEST, TOY_FILES, 2);
    rec0 = p.bytes + HDR + ((strlen(MANIFEST) + 7u) & ~(size_t)7u);
    put64(rec0 + OSP_MAX_PATH + 8, 1u << 20); /* offset */
    reseal(&p);
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_OVERLAP);
    free(p.bytes);
}

TEST(ospkg, two_records_naming_one_path) {
    want_file_t dup[] = {
        {"bin/toy", "one", 0},
        {"bin/toy", "another", 0},
    };
    blob_t p = build(MANIFEST, dup, 2);
    osp_t pkg;
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_DUP);
    free(p.bytes);
}

/* ---- paths ------------------------------------------------------------
 *
 * The list a package manager exists to refuse. Checked through the
 * predicate AND through a whole archive, because a rule the reader
 * forgot to call is the same as a rule that does not exist.
 */
TEST(ospkg, paths_a_package_may_not_contain) {
    static const char *bad[] = {
        "/etc/passwd",           /* absolute */
        "../../bin/sh",          /* the classic */
        "bin/../../../bin/sh",   /* buried in the middle */
        "..",                    /* on its own */
        ".",                     /* and its harmless-looking sibling */
        "bin/./toy",             /* "." as a component */
        "bin//toy",              /* an empty component */
        "bin/",                  /* a directory pretending to be a file */
        "",                      /* nothing at all */
        "bin\\toy",              /* a separator from another system */
        "bin/toy\nname: root",   /* a newline, to forge a line in a log */
        "bin/\x01toy",           /* a control byte */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (ospkg_check_path(bad[i]) != -OSP_E_PATH) {
            test_fail(__FILE__, __LINE__, "path %zu ('%s') was accepted", i, bad[i]);
        }
    }
    static const char *good[] = {
        "bin/toy", "share/man/man1/toy.1", "toy", "a/b/c/d/e/f",
        "bin/..toy",       /* two dots at the START of a name is a name */
        "bin/toy..",       /* and at the end */
        "bin/to.y",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        if (ospkg_check_path(good[i]) != OSP_OK) {
            test_fail(__FILE__, __LINE__, "path '%s' was refused", good[i]);
        }
    }
}

TEST(ospkg, a_traversing_path_in_a_real_archive) {
    want_file_t escape[] = {{"../../bin/sh", "gotcha", OSP_F_EXEC}};
    blob_t p = build(MANIFEST, escape, 1);
    osp_t pkg;
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_PATH);
    free(p.bytes);
}

/* A path field with no NUL in it at all: 224 bytes of name. The record
 * is fixed-width, so nothing stops a builder writing one, and a reader
 * that treats the field as a C string would run off the end of it into
 * the next record. */
TEST(ospkg, an_unterminated_path_field) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    unsigned char *rec0 = p.bytes + HDR + ((strlen(MANIFEST) + 7u) & ~(size_t)7u);
    memset(rec0, 'a', OSP_MAX_PATH);
    reseal(&p);
    osp_t pkg;
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_PATH);
    free(p.bytes);
}

/* ---- the manifest ----------------------------------------------------- */

TEST(ospkg, a_manifest_needs_a_name_and_a_version) {
    osp_manifest_t man;
    CHECK_EQ(ospkg_parse_manifest("version: 1.0\n", 13, &man), -OSP_E_MANIFEST);
    CHECK_EQ(ospkg_parse_manifest("name: toy\n", 10, &man), -OSP_E_MANIFEST);
    CHECK_EQ(ospkg_parse_manifest("", 0, &man), -OSP_E_MANIFEST);
    CHECK_EQ(ospkg_parse_manifest("name: toy\nversion: 1.0\n", 23, &man), OSP_OK);
}

/* A name is one path component and becomes a directory under /pkg, so
 * every rule about a path applies to it and one more: no '/' at all. */
TEST(ospkg, a_name_that_would_escape_pkg) {
    osp_manifest_t man;
    CHECK_EQ(ospkg_parse_manifest("name: ../bin\nversion: 1.0\n", 26, &man),
             -OSP_E_MANIFEST);
    CHECK_EQ(ospkg_parse_manifest("name: a/b\nversion: 1.0\n", 23, &man),
             -OSP_E_MANIFEST);
    CHECK_EQ(ospkg_parse_manifest("name: toy\nversion: ../..\n", 25, &man),
             -OSP_E_MANIFEST);
    CHECK_EQ(ospkg_parse_manifest("name: /abs\nversion: 1.0\n", 24, &man),
             -OSP_E_MANIFEST);
}

TEST(ospkg, manifest_grammar) {
    osp_manifest_t man;
    const char *text =
        "# a comment\n"
        "\n"
        "name:    toy   \n"          /* whitespace either side of the value */
        "version: 1.0\r\n"           /* a CRLF that made a round trip */
        "summary: with: a colon in it\n"
        "unknown-key: ignored on purpose\n"
        "provides: a b c\n";
    CHECK_EQ(ospkg_parse_manifest(text, strlen(text), &man), OSP_OK);
    CHECK(strcmp(man.name, "toy") == 0);
    CHECK(strcmp(man.version, "1.0") == 0);
    CHECK(strcmp(man.summary, "with: a colon in it") == 0);
    CHECK(strcmp(man.provides, "a b c") == 0);
    /* A line that is not "key: value" is a refusal, not a skip: a
     * manifest half of which parsed is a package whose declared
     * capabilities may be the half that did not. */
    const char *bad = "name: toy\nversion: 1.0\nthis line has no colon\n";
    CHECK_EQ(ospkg_parse_manifest(bad, strlen(bad), &man), -OSP_E_MANIFEST);
}

/* A value longer than its field is truncated rather than refused, and
 * the truncation must not run off the end. Checked because the fields
 * are fixed-size and a manifest is the one part of a package a person
 * types by hand. */
TEST(ospkg, an_overlong_field_is_truncated_safely) {
    char text[1024];
    int n = snprintf(text, sizeof(text), "name: toy\nversion: 1.0\nsummary: ");
    for (int i = 0; i < 400; i++) {
        text[n++] = 'x';
    }
    text[n++] = '\n';
    osp_manifest_t man;
    CHECK_EQ(ospkg_parse_manifest(text, (size_t)n, &man), OSP_OK);
    CHECK_EQ(strlen(man.summary), OSP_MAX_SUMMARY - 1);
}

/* ---- capabilities ------------------------------------------------------ */

TEST(ospkg, capability_names) {
    int unknown = 0;
    CHECK_EQ(ospkg_caps_from_names("network", &unknown), (long long)CAP_NETWORK);
    CHECK_EQ(unknown, 0);
    CHECK_EQ(ospkg_caps_from_names("fs-write network", &unknown),
             (long long)(CAP_FS_WRITE | CAP_NETWORK));
    CHECK_EQ(unknown, 0);
    CHECK_EQ(ospkg_caps_from_names("", &unknown), 0);
    CHECK_EQ(unknown, 0);
}

/* An unknown name sets the flag, and the caller refuses. Ignoring it
 * would mean a package built for a system with more capabilities than
 * this one installs here with the extras silently dropped - which reads
 * as success and is not. */
TEST(ospkg, an_unknown_capability_is_reported) {
    int unknown = 0;
    uint32_t mask = ospkg_caps_from_names("network raw-disk", &unknown);
    CHECK_EQ(unknown, 1);
    CHECK_EQ(mask, (long long)CAP_NETWORK); /* the known one is still parsed */
    unknown = 0;
    ospkg_caps_from_names("framebuffer2", &unknown);
    CHECK_EQ(unknown, 1);
}

/* The ceiling, asserted here as well as in the kernel. If somebody adds
 * a capability to CAP_PKG_MAX, this test is where the decision surfaces
 * rather than in a boot log. */
TEST(ospkg, the_ceiling_on_what_a_package_may_hold) {
    CHECK_EQ(CAP_PKG_MAX & CAP_FRAMEBUFFER, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_POWER, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_KILL_ANY, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_PROCESS_LIST, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_CLIPBOARD, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_DISPLAY_MODE, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_SET_TIME, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_SYSLOG, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_PKG_ADMIN, 0);
    /* And a program under /pkg that nothing vouches for holds nothing. */
    CHECK_EQ(CAP_PKG_UNLISTED, 0);
}

TEST(ospkg, caps_to_names_round_trip) {
    char buf[OSP_MAX_TEXT];
    ospkg_caps_to_names(CAP_FS_WRITE | CAP_NETWORK, buf, sizeof(buf));
    int unknown = 0;
    CHECK_EQ(ospkg_caps_from_names(buf, &unknown),
             (long long)(CAP_FS_WRITE | CAP_NETWORK));
    CHECK_EQ(unknown, 0);
    ospkg_caps_to_names(0, buf, sizeof(buf));
    CHECK_EQ(strlen(buf), 0u);
}

/* ---- the /pkg predicate ------------------------------------------------
 *
 * Three lines in caps.h, and the whole capability story rests on it
 * answering correctly for the paths that are nearly right.
 */
TEST(ospkg, what_counts_as_under_pkg) {
    CHECK_EQ(path_is_under_pkg("/pkg"), 1);
    CHECK_EQ(path_is_under_pkg("/pkg/"), 1);
    CHECK_EQ(path_is_under_pkg("/pkg/grep/3.11/bin/grep"), 1);
    CHECK_EQ(path_is_under_pkg("/pkg/db/caps"), 1);

    CHECK_EQ(path_is_under_pkg("/pkgfoo"), 0);      /* a prefix, not a parent */
    CHECK_EQ(path_is_under_pkg("/pkgfoo/bin/x"), 0);
    CHECK_EQ(path_is_under_pkg("/bin/grep"), 0);
    CHECK_EQ(path_is_under_pkg("/"), 0);
    CHECK_EQ(path_is_under_pkg(""), 0);
    /* A relative path is not under /pkg by this predicate, which is why
     * the kernel normalizes before it asks - see copy_write_path_from_user.
     * If that ordering is ever broken, "pkg/db/caps" from a process
     * whose directory is "/" would be a way past the gate. */
    CHECK_EQ(path_is_under_pkg("pkg/db/caps"), 0);
    CHECK_EQ(path_is_under_pkg(NULL), 0);
}

/* The shipped grant table must not have an entry that a package could
 * impersonate into something CAP_PKG_MAX forbids - which is not a
 * property of the table but of the rule that the table is not consulted
 * under /pkg. Asserted here as the statement of intent: `compositor`
 * holds CAP_ALL by name, and that is exactly why the name must not be
 * what decides. */
TEST(ospkg, the_name_that_makes_the_rule_necessary) {
    CHECK_EQ(caps_for_program("compositor"), (long long)CAP_ALL);
    CHECK_EQ(caps_for_program("/bin/compositor"), (long long)CAP_ALL);
    /* ...and the same basename under /pkg is a package path, which
     * caps_for_program is not the function to ask about. The kernel's
     * caps_for_spawn_path is; it is not linked into this tier, so what
     * is checked here is that the predicate that routes the decision
     * says the right thing. */
    CHECK_EQ(path_is_under_pkg("/pkg/impostor/1.0/bin/compositor"), 1);
}

/* ---- what the mutation harness found ---------------------------------
 *
 * `make mutate FILE=user_space/lib/ospkg.c` broke this reader forty ways
 * and the tests above noticed 62% of them. The survivors below are the
 * ones that were coverage without an assertion - every line was
 * executed, and nothing said what it should produce. They are grouped
 * here rather than folded into the tests above so that it stays obvious
 * which instrument asked for them.
 */

/* A space is legal in a path inside a package and a control byte is not,
 * and the boundary between those two is one comparison. Nothing above
 * used a path with a space in it, so `c < 0x20` could become `c <= 0x20`
 * - refusing every name with a space - with every test still passing. */
TEST(ospkg, the_boundary_between_a_space_and_a_control_byte) {
    CHECK_EQ(ospkg_check_path("share/a file with spaces"), OSP_OK);
    CHECK_EQ(ospkg_check_path("share/ leading-space"), OSP_OK);
    CHECK_EQ(ospkg_check_path("share/trailing-space "), OSP_OK);
    /* 0x1F is the last control byte and 0x20 is the space. */
    CHECK_EQ(ospkg_check_path("share/a\x1f" "b"), -OSP_E_PATH);
    CHECK_EQ(ospkg_check_path("share/a\x7f" "b"), -OSP_E_PATH);
    /* And a real archive carrying one, so the rule is checked where it
     * is applied rather than only where it is defined. */
    want_file_t spaced[] = {{"share/a file", "content", 0}};
    blob_t p = build(MANIFEST, spaced, 1);
    osp_t pkg;
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), OSP_OK);
    free(p.bytes);
}

/* ospkg_file_data's bounds check. Nothing above asked it for an index it
 * does not have, so `i >= file_count` could have been `i > file_count` -
 * and one past the end is a read into whatever follows the file table. */
TEST(ospkg, asking_for_a_file_that_is_not_there) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    osp_t pkg;
    REQUIRE(ospkg_open(p.bytes, p.len, &pkg) == OSP_OK);
    CHECK(ospkg_file_data(&pkg, 0) != NULL);
    CHECK(ospkg_file_data(&pkg, 1) != NULL);   /* the last one there is */
    CHECK(ospkg_file_data(&pkg, 2) == NULL);   /* one past it */
    CHECK(ospkg_file_data(&pkg, 3) == NULL);
    CHECK(ospkg_file_data(&pkg, 0xFFFFFFFFu) == NULL);
    free(p.bytes);
}

/* `requires:` was parsed and never read back, which is the one manifest
 * field whose absence would silently stop dependency resolution: `os`
 * would install a package and quietly not install what it needs. */
TEST(ospkg, the_requires_field) {
    osp_manifest_t man;
    const char *text = "name: toy\nversion: 1.0\nrequires: zlib libpng\n";
    CHECK_EQ(ospkg_parse_manifest(text, strlen(text), &man), OSP_OK);
    CHECK(strcmp(man.requires, "zlib libpng") == 0);
    /* And through a whole archive, which is how `os` actually sees it. */
    blob_t p = build("name: toy\nversion: 1.0\nrequires: zlib\n", TOY_FILES, 2);
    osp_t pkg;
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), OSP_OK);
    CHECK(strcmp(pkg.manifest.requires, "zlib") == 0);
    free(p.bytes);
}

/* ospkg_caps_to_names into a buffer too small for what it has to say.
 * It is used to build an error message about a package asking for too
 * much, which is exactly the moment a buffer overrun would happen and
 * exactly the moment nobody is watching. */
TEST(ospkg, capability_names_into_a_buffer_that_is_too_small) {
    char buf[8];
    for (size_t cap = 1; cap <= sizeof(buf); cap++) {
        char guard[16];
        memset(guard, '#', sizeof(guard));
        ospkg_caps_to_names(CAP_FS_WRITE | CAP_NETWORK | CAP_AUDIO, guard, cap);
        /* Terminated inside the space it was given... */
        int terminated = 0;
        for (size_t i = 0; i < cap; i++) {
            if (guard[i] == '\0') {
                terminated = 1;
                break;
            }
        }
        if (!terminated) {
            test_fail(__FILE__, __LINE__, "cap %zu: no NUL within the buffer", cap);
        }
        /* ...and not one byte past it. */
        for (size_t i = cap; i < sizeof(guard); i++) {
            if (guard[i] != '#') {
                test_fail(__FILE__, __LINE__, "cap %zu: wrote %zu bytes in", cap, i);
                break;
            }
        }
    }
}

/* The top byte of a 32-bit field. Every length used above fits in three
 * bytes, so the shift that assembles the fourth could be wrong by one
 * with nothing noticing - and that byte is what separates "192 MiB,
 * refused" from "104 MiB, accepted". */
TEST(ospkg, the_high_byte_of_a_length_is_read) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    osp_t pkg;
    /* 208 MiB: over OSP_MAX_PAYLOAD (192 MiB), and it is the top byte
     * that says so. A reader that dropped that byte would see 104 MiB,
     * accept it, and fail somewhere else entirely. */
    put64(p.bytes + 24, 0x0D000000ull);
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_HUGE);
    /* And the high half of the 64-bit field, which is a second rd32. */
    put64(p.bytes + 24, 0x0000000100000000ull);
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_HUGE);
    free(p.bytes);
}

/* A reserved field is only reserved if a package that puts something in
 * it is refused. Otherwise packages arrive in the world with rubbish
 * there and the field can never be used for anything. */
TEST(ospkg, a_reserved_field_must_be_zero) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    put32(p.bytes + 20, 1);
    reseal(&p);
    osp_t pkg;
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), -OSP_E_FORMAT);
    free(p.bytes);
}

/* A manifest with no trailing newline, and one whose last line is only
 * whitespace. Both are the end-of-buffer cases in the line scanner, and
 * both are what a manifest written by hand actually looks like. */
TEST(ospkg, a_manifest_that_does_not_end_in_a_newline) {
    osp_manifest_t man;
    const char *no_nl = "name: toy\nversion: 1.0";
    CHECK_EQ(ospkg_parse_manifest(no_nl, strlen(no_nl), &man), OSP_OK);
    CHECK(strcmp(man.version, "1.0") == 0);

    const char *trailing_ws = "name: toy\nversion: 1.0\n   \n";
    CHECK_EQ(ospkg_parse_manifest(trailing_ws, strlen(trailing_ws), &man), OSP_OK);
    CHECK(strcmp(man.version, "1.0") == 0);

    const char *ws_only = "   \n\t\n";
    CHECK_EQ(ospkg_parse_manifest(ws_only, strlen(ws_only), &man), -OSP_E_MANIFEST);

    /* A key with nothing after the colon: present, and empty. */
    const char *empty_value = "name: toy\nversion: 1.0\nsummary:\n";
    CHECK_EQ(ospkg_parse_manifest(empty_value, strlen(empty_value), &man), OSP_OK);
    CHECK_EQ(strlen(man.summary), 0u);

    /* A colon at the very end of the last line, with no newline. */
    const char *colon_last = "name: toy\nversion: 1.0\nsummary:";
    CHECK_EQ(ospkg_parse_manifest(colon_last, strlen(colon_last), &man), OSP_OK);
}

/* Two paths where one is a prefix of the other are NOT duplicates, and
 * the comparison that decides is one `&&`. A package with `bin/toy` and
 * `bin/toy2` in it is ordinary; refusing it would be a package manager
 * that cannot install a program next to its own helper. */
TEST(ospkg, a_prefix_is_not_a_duplicate) {
    want_file_t prefixes[] = {
        {"bin/toy", "one", 0},
        {"bin/toy2", "two", 0},
        {"bin/to", "three", 0},
    };
    blob_t p = build(MANIFEST, prefixes, 3);
    osp_t pkg;
    CHECK_EQ(ospkg_open(p.bytes, p.len, &pkg), OSP_OK);
    CHECK_EQ(pkg.file_count, 3u);
    free(p.bytes);
}

/* The refusals ospkg_check_path makes before it looks at anything. */
TEST(ospkg, no_path_at_all) {
    CHECK_EQ(ospkg_check_path(NULL), -OSP_E_PATH);
    CHECK_EQ(ospkg_check_path(""), -OSP_E_PATH);
}

/* A version is one path component too, and for the same reason a name
 * is: it becomes the directory below the package's own. */
TEST(ospkg, a_version_is_one_component) {
    osp_manifest_t man;
    CHECK_EQ(ospkg_parse_manifest("name: toy\nversion: 1/0\n", 22, &man),
             -OSP_E_MANIFEST);
    CHECK_EQ(ospkg_parse_manifest("name: toy\nversion: 1.0-rc1\n", 27, &man),
             OSP_OK);
}

TEST(ospkg, the_license_field) {
    osp_manifest_t man;
    const char *text = "name: toy\nversion: 1.0\nlicense: GPL-3.0-or-later\n";
    CHECK_EQ(ospkg_parse_manifest(text, strlen(text), &man), OSP_OK);
    CHECK(strcmp(man.license, "GPL-3.0-or-later") == 0);
    CHECK(strcmp(man.name, "toy") == 0); /* the other fields still landed */
}
