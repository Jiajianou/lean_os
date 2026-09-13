#include "check.h"

#include "os_package.h"
#include "sha256.h"
#include "capabilities.h"

#include <stdlib.h>
#include <string.h>

#define HEADER  OSP_HEADER_BYTES
#define REC  OSP_FILE_BYTES

typedef struct {
    unsigned char *bytes;
    size_t         length;
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

static blob_t build(const char *manifest, const want_file_t *files, int nfiles) {
    size_t meta_length = (strlen(manifest) + 7u) & ~(size_t)7u;
    size_t payload_length = 0;
    for (int i = 0; i < nfiles; i++) {
        payload_length += strlen(files[i].content);
    }
    size_t table_length = (size_t)nfiles * REC;
    size_t total = HEADER + meta_length + table_length + payload_length;

    unsigned char *b = calloc(total ? total : 1, 1);
    REQUIRE(b != NULL);

    memcpy(b, "LEANPKG1", 8);
    put32(b + 8, 1);
    put32(b + 12, (uint32_t)meta_length);
    put32(b + 16, (uint32_t)nfiles);
    put32(b + 20, 0);
    put64(b + 24, (uint64_t)payload_length);
    memcpy(b + HEADER, manifest, strlen(manifest));
    for (size_t i = strlen(manifest); i < meta_length; i++) {
        b[HEADER + i] = '\n';
    }

    unsigned char *table = b + HEADER + meta_length;
    unsigned char *payload = table + table_length;
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

    sha256(b + HEADER, total - HEADER, b + 32);

    blob_t out = {b, total};
    return out;
}

static void reseal(blob_t *p) {
    sha256(p->bytes + HEADER, p->length - HEADER, p->bytes + 32);
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

TEST(os_package, a_well_formed_package_opens) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    osp_t pkg;
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), OSP_OK);
    CHECK(strcmp(pkg.manifest.name, "toy") == 0);
    CHECK(strcmp(pkg.manifest.version, "1.0") == 0);
    CHECK_EQ(pkg.file_count, 2u);
    CHECK(strcmp(pkg.files[0].path, "bin/toy") == 0);
    CHECK_EQ(pkg.files[0].flags & OSP_F_EXEC, OSP_F_EXEC);
    CHECK(memcmp(os_package_file_data(&pkg, 1), "read me", 7) == 0);
    free(p.bytes);
}

TEST(os_package, not_a_package_at_all) {
    unsigned char junk[HEADER + 16];
    memset(junk, 0xAB, sizeof(junk));
    osp_t pkg;
    CHECK_EQ(os_package_open(junk, sizeof(junk), &pkg), -OSP_E_MAGIC);
    CHECK_EQ(os_package_open(junk, 8, &pkg), -OSP_E_SHORT);
    CHECK_EQ(os_package_open(junk, 0, &pkg), -OSP_E_SHORT);
}

TEST(os_package, a_format_from_the_future) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    put32(p.bytes + 8, 2);
    reseal(&p);
    osp_t pkg;
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_FORMAT);
    free(p.bytes);
}

TEST(os_package, one_flipped_bit_is_refused) {
    size_t probes[] = {HEADER, HEADER + 5, HEADER + 40, HEADER + 90, HEADER + 200, HEADER + 400};
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        blob_t p = build(MANIFEST, TOY_FILES, 2);
        REQUIRE(probes[i] < p.length);
        p.bytes[probes[i]] ^= 0x01;
        osp_t pkg;
        int rc = os_package_open(p.bytes, p.length, &pkg);
        if (rc != -OSP_E_BODY_HASH) {
            test_fail(__FILE__, __LINE__,
                      "a flipped bit at offset %zu gave %d, not -OSP_E_BODY_HASH",
                      probes[i], rc);
        }
        free(p.bytes);
    }
}

TEST(os_package, a_file_that_is_not_what_its_record_says) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    size_t payload = HEADER + ((strlen(MANIFEST) + 7u) & ~(size_t)7u) + 2 * REC;
    p.bytes[payload] = 'X';
    reseal(&p);
    osp_t pkg;
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_FILE_HASH);
    free(p.bytes);
}

TEST(os_package, truncated_after_the_header) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    osp_t pkg;
    for (size_t length = HEADER; length < p.length; length += 37) {
        int rc = os_package_open(p.bytes, length, &pkg);
        if (rc != -OSP_E_SHORT) {
            test_fail(__FILE__, __LINE__,
                      "a %zu-byte prefix of a %zu-byte package gave %d",
                      length, p.length, rc);
            break;
        }
    }
    CHECK_EQ(os_package_open(p.bytes, p.length - 1, &pkg), -OSP_E_SHORT);
    free(p.bytes);
}

TEST(os_package, lengths_that_would_wrap) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    osp_t pkg;

    put32(p.bytes + 16, 0x10000000u);
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_HUGE);

    put32(p.bytes + 16, 2);
    put64(p.bytes + 24, 0xFFFFFFFFFFFFFF00ull);
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_HUGE);

    put64(p.bytes + 24, 15);
    put32(p.bytes + 12, 0xFFFFFF00u);
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_HUGE);

    free(p.bytes);
}

TEST(os_package, a_file_that_points_outside_the_payload) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    unsigned char *rec0 = p.bytes + HEADER + ((strlen(MANIFEST) + 7u) & ~(size_t)7u);
    put64(rec0 + OSP_MAX_PATH, 1u << 20);
    reseal(&p);
    osp_t pkg;
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_OVERLAP);
    free(p.bytes);

    p = build(MANIFEST, TOY_FILES, 2);
    rec0 = p.bytes + HEADER + ((strlen(MANIFEST) + 7u) & ~(size_t)7u);
    put64(rec0 + OSP_MAX_PATH + 8, 1u << 20);
    reseal(&p);
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_OVERLAP);
    free(p.bytes);
}

TEST(os_package, two_records_naming_one_path) {
    want_file_t dup[] = {
        {"bin/toy", "one", 0},
        {"bin/toy", "another", 0},
    };
    blob_t p = build(MANIFEST, dup, 2);
    osp_t pkg;
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_DUP);
    free(p.bytes);
}

TEST(os_package, paths_a_package_may_not_contain) {
    static const char *bad[] = {
        "/etc/passwd",
        "../../bin/sh",
        "bin/../../../bin/sh",
        "..",
        ".",
        "bin/./toy",
        "bin//toy",
        "bin/",
        "",
        "bin\\toy",
        "bin/toy\nname: root",
        "bin/\x01toy",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (os_package_check_path(bad[i]) != -OSP_E_PATH) {
            test_fail(__FILE__, __LINE__, "path %zu ('%s') was accepted", i, bad[i]);
        }
    }
    static const char *good[] = {
        "bin/toy", "share/man/man1/toy.1", "toy", "a/b/c/d/e/f",
        "bin/..toy",
        "bin/toy..",
        "bin/to.y",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        if (os_package_check_path(good[i]) != OSP_OK) {
            test_fail(__FILE__, __LINE__, "path '%s' was refused", good[i]);
        }
    }
}

TEST(os_package, a_traversing_path_in_a_real_archive) {
    want_file_t escape[] = {{"../../bin/sh", "gotcha", OSP_F_EXEC}};
    blob_t p = build(MANIFEST, escape, 1);
    osp_t pkg;
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_PATH);
    free(p.bytes);
}

TEST(os_package, an_unterminated_path_field) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    unsigned char *rec0 = p.bytes + HEADER + ((strlen(MANIFEST) + 7u) & ~(size_t)7u);
    memset(rec0, 'a', OSP_MAX_PATH);
    reseal(&p);
    osp_t pkg;
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_PATH);
    free(p.bytes);
}

TEST(os_package, a_manifest_needs_a_name_and_a_version) {
    osp_manifest_t man;
    CHECK_EQ(os_package_parse_manifest("version: 1.0\n", 13, &man), -OSP_E_MANIFEST);
    CHECK_EQ(os_package_parse_manifest("name: toy\n", 10, &man), -OSP_E_MANIFEST);
    CHECK_EQ(os_package_parse_manifest("", 0, &man), -OSP_E_MANIFEST);
    CHECK_EQ(os_package_parse_manifest("name: toy\nversion: 1.0\n", 23, &man), OSP_OK);
}

TEST(os_package, a_name_that_would_escape_pkg) {
    osp_manifest_t man;
    CHECK_EQ(os_package_parse_manifest("name: ../bin\nversion: 1.0\n", 26, &man),
             -OSP_E_MANIFEST);
    CHECK_EQ(os_package_parse_manifest("name: a/b\nversion: 1.0\n", 23, &man),
             -OSP_E_MANIFEST);
    CHECK_EQ(os_package_parse_manifest("name: toy\nversion: ../..\n", 25, &man),
             -OSP_E_MANIFEST);
    CHECK_EQ(os_package_parse_manifest("name: /abs\nversion: 1.0\n", 24, &man),
             -OSP_E_MANIFEST);
}

TEST(os_package, manifest_grammar) {
    osp_manifest_t man;
    const char *text =
        "# a comment\n"
        "\n"
        "name:    toy   \n"
        "version: 1.0\r\n"
        "summary: with: a colon in it\n"
        "unknown-key: ignored on purpose\n"
        "provides: a b c\n";
    CHECK_EQ(os_package_parse_manifest(text, strlen(text), &man), OSP_OK);
    CHECK(strcmp(man.name, "toy") == 0);
    CHECK(strcmp(man.version, "1.0") == 0);
    CHECK(strcmp(man.summary, "with: a colon in it") == 0);
    CHECK(strcmp(man.provides, "a b c") == 0);
    const char *bad = "name: toy\nversion: 1.0\nthis line has no colon\n";
    CHECK_EQ(os_package_parse_manifest(bad, strlen(bad), &man), -OSP_E_MANIFEST);
}

TEST(os_package, an_overlong_field_is_truncated_safely) {
    char text[1024];
    int n = snprintf(text, sizeof(text), "name: toy\nversion: 1.0\nsummary: ");
    for (int i = 0; i < 400; i++) {
        text[n++] = 'x';
    }
    text[n++] = '\n';
    osp_manifest_t man;
    CHECK_EQ(os_package_parse_manifest(text, (size_t)n, &man), OSP_OK);
    CHECK_EQ(strlen(man.summary), OSP_MAX_SUMMARY - 1);
}

TEST(os_package, capability_names) {
    int unknown = 0;
    CHECK_EQ(os_package_caps_from_names("network", &unknown), (long long)CAP_NETWORK);
    CHECK_EQ(unknown, 0);
    CHECK_EQ(os_package_caps_from_names("fs-write network", &unknown),
             (long long)(CAP_FS_WRITE | CAP_NETWORK));
    CHECK_EQ(unknown, 0);
    CHECK_EQ(os_package_caps_from_names("", &unknown), 0);
    CHECK_EQ(unknown, 0);
}

TEST(os_package, an_unknown_capability_is_reported) {
    int unknown = 0;
    uint32_t mask = os_package_caps_from_names("network raw-disk", &unknown);
    CHECK_EQ(unknown, 1);
    CHECK_EQ(mask, (long long)CAP_NETWORK);
    unknown = 0;
    os_package_caps_from_names("framebuffer2", &unknown);
    CHECK_EQ(unknown, 1);
}

TEST(os_package, the_ceiling_on_what_a_package_may_hold) {
    CHECK_EQ(CAP_PKG_MAX & CAP_FRAMEBUFFER, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_POWER, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_KILL_ANY, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_PROCESS_LIST, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_CLIPBOARD, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_DISPLAY_MODE, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_SET_TIME, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_SYSLOG, 0);
    CHECK_EQ(CAP_PKG_MAX & CAP_PKG_ADMIN, 0);
    CHECK_EQ(CAP_PKG_UNLISTED, 0);
}

TEST(os_package, caps_to_names_round_trip) {
    char buffer[OSP_MAX_TEXT];
    os_package_caps_to_names(CAP_FS_WRITE | CAP_NETWORK, buffer, sizeof(buffer));
    int unknown = 0;
    CHECK_EQ(os_package_caps_from_names(buffer, &unknown),
             (long long)(CAP_FS_WRITE | CAP_NETWORK));
    CHECK_EQ(unknown, 0);
    os_package_caps_to_names(0, buffer, sizeof(buffer));
    CHECK_EQ(strlen(buffer), 0u);
}

TEST(os_package, what_counts_as_under_pkg) {
    CHECK_EQ(path_is_under_pkg("/pkg"), 1);
    CHECK_EQ(path_is_under_pkg("/pkg/"), 1);
    CHECK_EQ(path_is_under_pkg("/pkg/grep/3.11/bin/grep"), 1);
    CHECK_EQ(path_is_under_pkg("/pkg/db/caps"), 1);

    CHECK_EQ(path_is_under_pkg("/pkgfoo"), 0);
    CHECK_EQ(path_is_under_pkg("/pkgfoo/bin/x"), 0);
    CHECK_EQ(path_is_under_pkg("/bin/grep"), 0);
    CHECK_EQ(path_is_under_pkg("/"), 0);
    CHECK_EQ(path_is_under_pkg(""), 0);
    CHECK_EQ(path_is_under_pkg("pkg/db/caps"), 0);
    CHECK_EQ(path_is_under_pkg(NULL), 0);
}

TEST(os_package, the_name_that_makes_the_rule_necessary) {
    CHECK_EQ(caps_for_program("compositor"), (long long)CAP_ALL);
    CHECK_EQ(caps_for_program("/bin/compositor"), (long long)CAP_ALL);
    CHECK_EQ(path_is_under_pkg("/pkg/impostor/1.0/bin/compositor"), 1);
}

TEST(os_package, the_boundary_between_a_space_and_a_control_byte) {
    CHECK_EQ(os_package_check_path("share/a file with spaces"), OSP_OK);
    CHECK_EQ(os_package_check_path("share/ leading-space"), OSP_OK);
    CHECK_EQ(os_package_check_path("share/trailing-space "), OSP_OK);
    CHECK_EQ(os_package_check_path("share/a\x1f" "b"), -OSP_E_PATH);
    CHECK_EQ(os_package_check_path("share/a\x7f" "b"), -OSP_E_PATH);
    want_file_t spaced[] = {{"share/a file", "content", 0}};
    blob_t p = build(MANIFEST, spaced, 1);
    osp_t pkg;
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), OSP_OK);
    free(p.bytes);
}

TEST(os_package, asking_for_a_file_that_is_not_there) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    osp_t pkg;
    REQUIRE(os_package_open(p.bytes, p.length, &pkg) == OSP_OK);
    CHECK(os_package_file_data(&pkg, 0) != NULL);
    CHECK(os_package_file_data(&pkg, 1) != NULL);
    CHECK(os_package_file_data(&pkg, 2) == NULL);
    CHECK(os_package_file_data(&pkg, 3) == NULL);
    CHECK(os_package_file_data(&pkg, 0xFFFFFFFFu) == NULL);
    free(p.bytes);
}

TEST(os_package, the_requires_field) {
    osp_manifest_t man;
    const char *text = "name: toy\nversion: 1.0\nrequires: zlib libpng\n";
    CHECK_EQ(os_package_parse_manifest(text, strlen(text), &man), OSP_OK);
    CHECK(strcmp(man.requires, "zlib libpng") == 0);
    blob_t p = build("name: toy\nversion: 1.0\nrequires: zlib\n", TOY_FILES, 2);
    osp_t pkg;
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), OSP_OK);
    CHECK(strcmp(pkg.manifest.requires, "zlib") == 0);
    free(p.bytes);
}

TEST(os_package, capability_names_into_a_buffer_that_is_too_small) {
    char buffer[8];
    for (size_t cap = 1; cap <= sizeof(buffer); cap++) {
        char guard[16];
        memset(guard, '#', sizeof(guard));
        os_package_caps_to_names(CAP_FS_WRITE | CAP_NETWORK | CAP_AUDIO, guard, cap);
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
        for (size_t i = cap; i < sizeof(guard); i++) {
            if (guard[i] != '#') {
                test_fail(__FILE__, __LINE__, "cap %zu: wrote %zu bytes in", cap, i);
                break;
            }
        }
    }
}

TEST(os_package, the_high_byte_of_a_length_is_read) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    osp_t pkg;
    put64(p.bytes + 24, 0x0D000000ull);
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_HUGE);
    put64(p.bytes + 24, 0x0000000100000000ull);
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_HUGE);
    free(p.bytes);
}

TEST(os_package, a_reserved_field_must_be_zero) {
    blob_t p = build(MANIFEST, TOY_FILES, 2);
    put32(p.bytes + 20, 1);
    reseal(&p);
    osp_t pkg;
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), -OSP_E_FORMAT);
    free(p.bytes);
}

TEST(os_package, a_manifest_that_does_not_end_in_a_newline) {
    osp_manifest_t man;
    const char *no_nl = "name: toy\nversion: 1.0";
    CHECK_EQ(os_package_parse_manifest(no_nl, strlen(no_nl), &man), OSP_OK);
    CHECK(strcmp(man.version, "1.0") == 0);

    const char *trailing_ws = "name: toy\nversion: 1.0\n   \n";
    CHECK_EQ(os_package_parse_manifest(trailing_ws, strlen(trailing_ws), &man), OSP_OK);
    CHECK(strcmp(man.version, "1.0") == 0);

    const char *ws_only = "   \n\t\n";
    CHECK_EQ(os_package_parse_manifest(ws_only, strlen(ws_only), &man), -OSP_E_MANIFEST);

    const char *empty_value = "name: toy\nversion: 1.0\nsummary:\n";
    CHECK_EQ(os_package_parse_manifest(empty_value, strlen(empty_value), &man), OSP_OK);
    CHECK_EQ(strlen(man.summary), 0u);

    const char *colon_last = "name: toy\nversion: 1.0\nsummary:";
    CHECK_EQ(os_package_parse_manifest(colon_last, strlen(colon_last), &man), OSP_OK);
}

TEST(os_package, a_prefix_is_not_a_duplicate) {
    want_file_t prefixes[] = {
        {"bin/toy", "one", 0},
        {"bin/toy2", "two", 0},
        {"bin/to", "three", 0},
    };
    blob_t p = build(MANIFEST, prefixes, 3);
    osp_t pkg;
    CHECK_EQ(os_package_open(p.bytes, p.length, &pkg), OSP_OK);
    CHECK_EQ(pkg.file_count, 3u);
    free(p.bytes);
}

TEST(os_package, no_path_at_all) {
    CHECK_EQ(os_package_check_path(NULL), -OSP_E_PATH);
    CHECK_EQ(os_package_check_path(""), -OSP_E_PATH);
}

TEST(os_package, a_version_is_one_component) {
    osp_manifest_t man;
    CHECK_EQ(os_package_parse_manifest("name: toy\nversion: 1/0\n", 22, &man),
             -OSP_E_MANIFEST);
    CHECK_EQ(os_package_parse_manifest("name: toy\nversion: 1.0-rc1\n", 27, &man),
             OSP_OK);
}

TEST(os_package, the_license_field) {
    osp_manifest_t man;
    const char *text = "name: toy\nversion: 1.0\nlicense: GPL-3.0-or-later\n";
    CHECK_EQ(os_package_parse_manifest(text, strlen(text), &man), OSP_OK);
    CHECK(strcmp(man.license, "GPL-3.0-or-later") == 0);
    CHECK(strcmp(man.name, "toy") == 0);
}
