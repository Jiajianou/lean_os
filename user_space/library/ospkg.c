#include "ospkg.h"

#include "caps.h"

static size_t osp_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

const char *osp_strerror(int err) {
    if (err < 0) {
        err = -err;
    }
    switch (err) {
    case OSP_OK:          return "ok";
    case OSP_E_SHORT:     return "the file ends before the format says it should";
    case OSP_E_MAGIC:     return "not a lean_os package";
    case OSP_E_FORMAT:    return "a package format this machine cannot read";
    case OSP_E_HUGE:      return "the package declares more than this machine will accept";
    case OSP_E_BODY_HASH: return "these are not the bytes that were built - the archive hash does not match";
    case OSP_E_FILE_HASH: return "a file's content does not match its recorded hash";
    case OSP_E_PATH:      return "a path a package is not allowed to contain";
    case OSP_E_OVERLAP:   return "a file record points outside the payload";
    case OSP_E_MANIFEST:  return "the manifest has no name, no version, or a line that is not 'key: value'";
    case OSP_E_DUP:       return "two files in the package have the same path";
    default:              return "refused";
    }
}

int ospkg_check_path(const char *path) {
    if (!path || !path[0]) {
        return -OSP_E_PATH;
    }
    size_t n = 0;
    while (path[n] && n < OSP_MAX_PATH) {
        n++;
    }
    if (n >= OSP_MAX_PATH) {
        return -OSP_E_PATH;
    }
    if (path[0] == '/' || path[n - 1] == '/') {
        return -OSP_E_PATH;
    }
    size_t comp = 0;
    for (size_t i = 0; i <= n; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c == '/' || c == '\0') {
            if (comp == 0) {
                return -OSP_E_PATH;
            }
            if (comp == 1 && path[i - 1] == '.') {
                return -OSP_E_PATH;
            }
            if (comp == 2 && path[i - 1] == '.' && path[i - 2] == '.') {
                return -OSP_E_PATH;
            }
            comp = 0;
            continue;
        }
        if (c < 0x20 || c == 0x7F || c == '\\') {
            return -OSP_E_PATH;
        }
        comp++;
    }
    return OSP_OK;
}

static void copy_field(char *dst, size_t cap, const char *src, size_t len) {
    size_t i = 0;
    for (; i < len && i < cap - 1; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static int field_matches(const char *key, size_t keylen, const char *name) {
    size_t n = osp_strlen(name);
    if (n != keylen) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (key[i] != name[i]) {
            return 0;
        }
    }
    return 1;
}

int ospkg_parse_manifest(const char *text, size_t len, osp_manifest_t *out) {
    for (size_t i = 0; i < sizeof(*out); i++) {
        ((char *)out)[i] = '\0';
    }

    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') {
            i++;
        }
        size_t end = i;
        if (i < len) {
            i++;
        }
        if (end > start && text[end - 1] == '\r') {
            end--;
        }
        while (start < end && (text[start] == ' ' || text[start] == '\t')) {
            start++;
        }
        if (start >= end || text[start] == '#') {
            continue;
        }

        size_t colon = start;
        while (colon < end && text[colon] != ':') {
            colon++;
        }
        if (colon >= end) {
            return -OSP_E_MANIFEST;
        }
        const char *key = text + start;
        size_t keylen = colon - start;
        size_t vstart = colon + 1;
        while (vstart < end && (text[vstart] == ' ' || text[vstart] == '\t')) {
            vstart++;
        }
        size_t vend = end;
        while (vend > vstart && (text[vend - 1] == ' ' || text[vend - 1] == '\t')) {
            vend--;
        }
        const char *val = text + vstart;
        size_t vlen = vend - vstart;

        if (field_matches(key, keylen, "name")) {
            copy_field(out->name, sizeof(out->name), val, vlen);
        } else if (field_matches(key, keylen, "version")) {
            copy_field(out->version, sizeof(out->version), val, vlen);
        } else if (field_matches(key, keylen, "summary")) {
            copy_field(out->summary, sizeof(out->summary), val, vlen);
        } else if (field_matches(key, keylen, "requires")) {
            copy_field(out->requires, sizeof(out->requires), val, vlen);
        } else if (field_matches(key, keylen, "provides")) {
            copy_field(out->provides, sizeof(out->provides), val, vlen);
        } else if (field_matches(key, keylen, "caps")) {
            copy_field(out->caps, sizeof(out->caps), val, vlen);
        } else if (field_matches(key, keylen, "source")) {
            copy_field(out->source, sizeof(out->source), val, vlen);
        } else if (field_matches(key, keylen, "license")) {
            copy_field(out->license, sizeof(out->license), val, vlen);
        }
    }

    if (!out->name[0] || !out->version[0]) {
        return -OSP_E_MANIFEST;
    }
    if (ospkg_check_path(out->name) != OSP_OK ||
        ospkg_check_path(out->version) != OSP_OK) {
        return -OSP_E_MANIFEST;
    }
    for (const char *p = out->name; *p; p++) {
        if (*p == '/') {
            return -OSP_E_MANIFEST;
        }
    }
    for (const char *p = out->version; *p; p++) {
        if (*p == '/') {
            return -OSP_E_MANIFEST;
        }
    }
    return OSP_OK;
}

int ospkg_open(const uint8_t *bytes, size_t len, osp_t *out) {
    if (len < OSP_HEADER_BYTES) {
        return -OSP_E_SHORT;
    }
    static const uint8_t want[8] = {OSP_MAGIC0, OSP_MAGIC1, OSP_MAGIC2, OSP_MAGIC3,
                                    OSP_MAGIC4, OSP_MAGIC5, OSP_MAGIC6, OSP_MAGIC7};
    for (int i = 0; i < 8; i++) {
        if (bytes[i] != want[i]) {
            return -OSP_E_MAGIC;
        }
    }

    osp_header_t h;
    for (int i = 0; i < 8; i++) {
        h.magic[i] = bytes[i];
    }
    h.format        = rd32(bytes + 8);
    h.meta_bytes    = rd32(bytes + 12);
    h.file_count    = rd32(bytes + 16);
    h.reserved      = rd32(bytes + 20);
    h.payload_bytes = rd64(bytes + 24);
    for (int i = 0; i < SHA256_DIGEST_BYTES; i++) {
        h.body_sha256[i] = bytes[32 + i];
    }

    if (h.format != 1) {
        return -OSP_E_FORMAT;
    }
    if ((h.meta_bytes & 7u) != 0) {
        return -OSP_E_FORMAT;
    }
    if (((uintptr_t)bytes & 7u) != 0) {
        return -OSP_E_FORMAT;
    }
    if (h.file_count > OSP_MAX_FILES || h.payload_bytes > OSP_MAX_PAYLOAD ||
        h.meta_bytes > 64u * 1024u) {
        return -OSP_E_HUGE;
    }
    if (h.reserved != 0) {
        return -OSP_E_FORMAT;
    }

    uint64_t table_bytes = (uint64_t)h.file_count * (uint64_t)OSP_FILE_BYTES;
    uint64_t need = (uint64_t)OSP_HEADER_BYTES + (uint64_t)h.meta_bytes +
                    table_bytes + h.payload_bytes;
    if (need > (uint64_t)len) {
        return -OSP_E_SHORT;
    }

    uint8_t digest[SHA256_DIGEST_BYTES];
    sha256(bytes + OSP_HEADER_BYTES, (size_t)(need - OSP_HEADER_BYTES), digest);
    if (!sha256_equal(digest, h.body_sha256)) {
        return -OSP_E_BODY_HASH;
    }

    const uint8_t *meta = bytes + OSP_HEADER_BYTES;
    const uint8_t *table = meta + h.meta_bytes;
    const uint8_t *payload = table + table_bytes;

    osp_manifest_t man;
    int rc = ospkg_parse_manifest((const char *)meta, h.meta_bytes, &man);
    if (rc != OSP_OK) {
        return rc;
    }

    const osp_file_t *files = (const osp_file_t *)(const void *)table;

    for (uint32_t i = 0; i < h.file_count; i++) {
        const osp_file_t *f = &files[i];
        if (ospkg_check_path(f->path) != OSP_OK) {
            return -OSP_E_PATH;
        }
        uint64_t off = f->offset;
        uint64_t sz = f->size;
        if (off > h.payload_bytes || sz > h.payload_bytes ||
            off + sz > h.payload_bytes) {
            return -OSP_E_OVERLAP;
        }
        for (uint32_t j = 0; j < i; j++) {
            const char *a = f->path, *b = files[j].path;
            size_t k = 0;
            while (a[k] && a[k] == b[k]) {
                k++;
            }
            if (a[k] == '\0' && b[k] == '\0') {
                return -OSP_E_DUP;
            }
        }
        uint8_t fd[SHA256_DIGEST_BYTES];
        sha256(payload + off, (size_t)sz, fd);
        if (!sha256_equal(fd, f->sha256)) {
            return -OSP_E_FILE_HASH;
        }
    }

    out->bytes = bytes;
    out->len = len;
    out->header = h;
    out->manifest = man;
    out->files = files;
    out->file_count = h.file_count;
    out->payload = payload;
    out->payload_bytes = h.payload_bytes;
    return OSP_OK;
}

const uint8_t *ospkg_file_data(const osp_t *pkg, uint32_t i) {
    if (i >= pkg->file_count) {
        return (const uint8_t *)0;
    }
    return pkg->payload + pkg->files[i].offset;
}

uint32_t ospkg_caps_from_names(const char *names, int *unknown_out) {
    uint32_t mask = 0;
    if (unknown_out) {
        *unknown_out = 0;
    }
    size_t i = 0;
    while (names[i]) {
        while (names[i] == ' ' || names[i] == '\t' || names[i] == ',') {
            i++;
        }
        if (!names[i]) {
            break;
        }
        size_t start = i;
        while (names[i] && names[i] != ' ' && names[i] != '\t' && names[i] != ',') {
            i++;
        }
        size_t n = i - start;
        int found = 0;
        for (int k = 0; k < CAP_NAME_COUNT; k++) {
            if (field_matches(names + start, n, CAP_NAMES[k].name)) {
                mask |= CAP_NAMES[k].bit;
                found = 1;
                break;
            }
        }
        if (!found && unknown_out) {
            *unknown_out = 1;
        }
    }
    return mask;
}

void ospkg_caps_to_names(uint32_t caps, char *out, size_t cap) {
    size_t n = 0;
    for (int k = 0; k < CAP_NAME_COUNT; k++) {
        if (!(caps & CAP_NAMES[k].bit)) {
            continue;
        }
        const char *s = CAP_NAMES[k].name;
        size_t sl = osp_strlen(s);
        if (n && n + 1 < cap) {
            out[n++] = ' ';
        }
        for (size_t i = 0; i < sl && n + 1 < cap; i++) {
            out[n++] = s[i];
        }
    }
    if (cap) {
        out[n < cap ? n : cap - 1] = '\0';
    }
}

_Static_assert(sizeof(osp_file_t) == OSP_FILE_BYTES, "osp_file_t is the wire record");
_Static_assert(sizeof(osp_header_t) == OSP_HEADER_BYTES, "osp_header_t is the wire header");
