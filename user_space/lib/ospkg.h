/* user_space/lib/ospkg.h - M111: what a package is.
 *
 * ---- the one design decision this format makes ------------------------
 *
 * **There are no install scripts.** Not a shell hook, not a "post-install
 * action", not a declarative rule that runs a program. A package is a
 * manifest, a file table and a payload, and installing it is a copy - so
 * the answer to "what can installing this package do to my machine" is a
 * sentence rather than an audit: it writes the files it lists, under a
 * directory named after itself, and nothing else runs.
 *
 * Every general-purpose package manager in the world has the hook and
 * regrets it. The reason they have it is that a package sometimes needs
 * to compile something, register something, or fix up a database - and
 * on this machine those are the package MANAGER's jobs, done by code
 * that shipped with the OS and can be read once, rather than by code
 * that arrived with the package and has to be read every time.
 *
 * If a package one day genuinely cannot be installed by copying, that is
 * a fact worth discovering as a refusal rather than papering over with a
 * mechanism nothing needs yet. M63's rule: the port that needs it pays
 * for it.
 *
 * ---- the layout -------------------------------------------------------
 *
 *   osp_header_t                  fixed, 96 bytes, little-endian
 *   manifest text                 meta_bytes, "key: value" lines
 *   osp_file_t[file_count]        one record per file
 *   payload                       every file's bytes, back to back
 *
 * The header carries a SHA-256 over everything after it, so a package is
 * one comparison away from "these are the bytes somebody built". Each
 * file record carries its own SHA-256 as well, which is not redundant:
 * the whole-archive hash answers "did this arrive intact", and the
 * per-file hashes answer "is what is on the disk now still what was
 * installed" - a question `os verify` asks months later, about files the
 * archive is no longer around to compare against.
 *
 * ---- what a path in a package may be ----------------------------------
 *
 * Relative, no leading '/', no "." or ".." component, no backslash, no
 * empty component, no control byte, at most OSP_MAX_PATH-1 characters.
 * Checked in ospkg_check_path() and applied by the reader BEFORE anything
 * is created, because a package manager that unpacks first and validates
 * second is the one with the directory-traversal bug in it. There is no
 * flag to relax this.
 *
 * ---- endianness, and alignment ----------------------------------------
 *
 * Little-endian on the wire, which is what both ends already are. Stated
 * rather than assumed, and the reader does the byte assembly explicitly
 * so that this file is honest on a big-endian host if one ever appears -
 * the same discipline leanfs_format.h uses.
 *
 * **The manifest is padded with newlines to a multiple of 8 bytes**, and
 * that is part of the format rather than tidiness. The header is 96
 * bytes and a file record is 280, both multiples of 8, so padding the
 * one variable-length region between them means the file table always
 * begins 8-aligned - which is what lets the reader use the records where
 * they lie instead of copying 1.1 MB of them somewhere else.
 *
 * It was not padded first time round, and UBSan said so on the first
 * test that ran: "member access within misaligned address ... requires
 * 8 byte alignment". x86-64 would have executed it quite happily, which
 * is precisely why that instrument exists. A reader that must not
 * allocate and a record that must not be copied leave alignment as the
 * thing to fix, and a format is the right place to fix it.
 *
 * The corollary is a requirement on the CALLER: the buffer handed to
 * ospkg_open must itself be 8-aligned. Both callers get theirs from
 * malloc, which is; the reader checks rather than trusts.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

#define OSP_MAGIC0 'L'
#define OSP_MAGIC1 'E'
#define OSP_MAGIC2 'A'
#define OSP_MAGIC3 'N'
#define OSP_MAGIC4 'P'
#define OSP_MAGIC5 'K'
#define OSP_MAGIC6 'G'
#define OSP_MAGIC7 '1'

#define OSP_MAX_PATH   224   /* inside a package, relative */
#define OSP_MAX_NAME    64   /* a package's own name */
#define OSP_MAX_VERSION 32
#define OSP_MAX_SUMMARY 128
#define OSP_MAX_TEXT    256  /* requires:, provides:, caps: - each one line */

/* A ceiling on everything, so that a corrupt or hostile header cannot ask
 * this machine for an allocation it will not survive. Both numbers are
 * far above anything real: grep is 640 files short of the first and 30
 * MiB short of the second. */
#define OSP_MAX_FILES   4096
#define OSP_MAX_PAYLOAD (192u * 1024u * 1024u)

/* File record flags. */
#define OSP_F_EXEC    (1u << 0) /* an executable; the installer marks it so */
#define OSP_F_SYMLINK (1u << 1) /* payload bytes are the link target, not content */

typedef struct {
    uint8_t  magic[8];
    uint32_t format;        /* 1 */
    uint32_t meta_bytes;
    uint32_t file_count;
    uint32_t reserved;
    uint64_t payload_bytes;
    uint8_t  body_sha256[SHA256_DIGEST_BYTES]; /* manifest + table + payload */
    uint8_t  pad[32];
} osp_header_t;

#define OSP_HEADER_BYTES 96

typedef struct {
    char     path[OSP_MAX_PATH];
    uint64_t size;
    uint64_t offset;        /* into the payload */
    uint32_t flags;
    uint32_t reserved;
    uint8_t  sha256[SHA256_DIGEST_BYTES];
} osp_file_t;

#define OSP_FILE_BYTES 280

/* The manifest, parsed. Fixed-size fields throughout: this is read on a
 * machine where a malformed package must not be able to ask for memory,
 * and a package whose summary is longer than 127 characters is a package
 * with a bad summary rather than a reason to allocate. */
typedef struct {
    char name[OSP_MAX_NAME];
    char version[OSP_MAX_VERSION];
    char summary[OSP_MAX_SUMMARY];
    char requires[OSP_MAX_TEXT];  /* space-separated package names */
    char provides[OSP_MAX_TEXT];  /* space-separated command names */
    char caps[OSP_MAX_TEXT];      /* space-separated capability names */
    char source[OSP_MAX_TEXT];    /* where the upstream came from */
    char license[OSP_MAX_NAME];
} osp_manifest_t;

/* A package held whole in memory, with pointers into the caller's buffer.
 * Nothing here owns anything. */
typedef struct {
    const uint8_t  *bytes;
    size_t          len;
    osp_header_t    header;
    osp_manifest_t  manifest;
    const osp_file_t *files;   /* file_count records */
    uint32_t        file_count;
    const uint8_t  *payload;
    uint64_t        payload_bytes;
} osp_t;

/* Why it was refused. Returned negated from the functions below; every
 * one of these is a distinct sentence a person can act on, which is the
 * whole reason it is an enum rather than -1. */
enum {
    OSP_OK = 0,
    OSP_E_SHORT = 1,     /* the file ends before the format says it should */
    OSP_E_MAGIC,         /* not a package */
    OSP_E_FORMAT,        /* a package of a version this cannot read */
    OSP_E_HUGE,          /* over one of the ceilings above */
    OSP_E_BODY_HASH,     /* the bytes are not the bytes that were built */
    OSP_E_FILE_HASH,     /* one file's content is not what its record says */
    OSP_E_PATH,          /* a path a package may not contain */
    OSP_E_OVERLAP,       /* a file record points outside the payload */
    OSP_E_MANIFEST,      /* no name, no version, or an unreadable line */
    OSP_E_DUP,           /* two records name the same path */
    OSP_E_COUNT
};

const char *osp_strerror(int err);

/* Is `path` one a package is allowed to contain? Returns OSP_OK or
 * -OSP_E_PATH. Exposed because the installer checks it a second time, on
 * the joined destination path, and a rule with one caller is a rule that
 * gets moved. */
int ospkg_check_path(const char *path);

/* Parse and fully verify `bytes`. On success `out` points into `bytes`,
 * which the caller must keep alive. Verifies, in this order: the magic,
 * the format, every ceiling, the whole-body hash, the manifest, every
 * path, every extent, and every per-file hash. Nothing is believed
 * before the body hash is checked except the numbers needed to check it.
 *
 * Returns OSP_OK or -OSP_E_*. */
int ospkg_open(const uint8_t *bytes, size_t len, osp_t *out);

/* The i'th file's content within the payload. Returns NULL if `i` is out
 * of range. For a symlink the bytes are the target, unterminated. */
const uint8_t *ospkg_file_data(const osp_t *pkg, uint32_t i);

/* Parse manifest text on its own (the repository index uses the same
 * "key: value" grammar). Returns OSP_OK or -OSP_E_MANIFEST. */
int ospkg_parse_manifest(const char *text, size_t len, osp_manifest_t *out);

/* ---- capabilities a package may ask for -------------------------------
 *
 * The names here are caps.h's names, and the mask this returns is what
 * the package manager writes into the registry the kernel reads. It is
 * intersected with CAP_PKG_MAX there, twice - once by `os` so that the
 * refusal has a message attached, and once by the kernel so that the
 * refusal does not depend on `os` being the thing that wrote the file.
 *
 * Returns the mask, and sets *unknown_out (if non-NULL) to 1 if any name
 * in the string was not recognised - which is refused rather than
 * ignored, because a capability name this OS does not know is either a
 * typo or a package built for a machine that is not this one. */
uint32_t ospkg_caps_from_names(const char *names, int *unknown_out);

/* The reverse, into `out` (at least OSP_MAX_TEXT bytes). */
void ospkg_caps_to_names(uint32_t caps, char *out, size_t cap);
