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

#define OSP_MAX_PATH   224
#define OSP_MAX_NAME    64
#define OSP_MAX_VERSION 32
#define OSP_MAX_SUMMARY 128
#define OSP_MAX_TEXT    256

#define OSP_MAX_FILES   4096
#define OSP_MAX_PAYLOAD (192u * 1024u * 1024u)

#define OSP_F_EXEC    (1u << 0)
#define OSP_F_SYMLINK (1u << 1)

typedef struct {
    uint8_t  magic[8];
    uint32_t format;
    uint32_t meta_bytes;
    uint32_t file_count;
    uint32_t reserved;
    uint64_t payload_bytes;
    uint8_t  body_sha256[SHA256_DIGEST_BYTES];
    uint8_t  pad[32];
} osp_header_t;

#define OSP_HEADER_BYTES 96

typedef struct {
    char     path[OSP_MAX_PATH];
    uint64_t size;
    uint64_t offset;
    uint32_t flags;
    uint32_t reserved;
    uint8_t  sha256[SHA256_DIGEST_BYTES];
} osp_file_t;

#define OSP_FILE_BYTES 280

typedef struct {
    char name[OSP_MAX_NAME];
    char version[OSP_MAX_VERSION];
    char summary[OSP_MAX_SUMMARY];
    char requires[OSP_MAX_TEXT];
    char provides[OSP_MAX_TEXT];
    char caps[OSP_MAX_TEXT];
    char source[OSP_MAX_TEXT];
    char license[OSP_MAX_NAME];
} osp_manifest_t;

typedef struct {
    const uint8_t  *bytes;
    size_t          len;
    osp_header_t    header;
    osp_manifest_t  manifest;
    const osp_file_t *files;
    uint32_t        file_count;
    const uint8_t  *payload;
    uint64_t        payload_bytes;
} osp_t;

enum {
    OSP_OK = 0,
    OSP_E_SHORT = 1,
    OSP_E_MAGIC,
    OSP_E_FORMAT,
    OSP_E_HUGE,
    OSP_E_BODY_HASH,
    OSP_E_FILE_HASH,
    OSP_E_PATH,
    OSP_E_OVERLAP,
    OSP_E_MANIFEST,
    OSP_E_DUP,
    OSP_E_COUNT
};

const char *osp_strerror(int err);

int ospkg_check_path(const char *path);

int ospkg_open(const uint8_t *bytes, size_t len, osp_t *out);

const uint8_t *ospkg_file_data(const osp_t *pkg, uint32_t i);

int ospkg_parse_manifest(const char *text, size_t len, osp_manifest_t *out);

uint32_t ospkg_caps_from_names(const char *names, int *unknown_out);

void ospkg_caps_to_names(uint32_t caps, char *out, size_t cap);
