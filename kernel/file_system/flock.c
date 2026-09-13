#include "flock.h"

typedef struct {
    uint8_t used;
    uint8_t type;
    int pid;
    uint32_t ino;
    uint64_t start;
    uint64_t end;
} flock_entry_t;

#define FLOCK_EOF UINT64_MAX

static flock_entry_t table[FLOCK_MAX];

static uint64_t range_end(uint64_t start, uint64_t len) {
    if (len == 0 || start > FLOCK_EOF - len) {
        return FLOCK_EOF;
    }
    return start + len;
}

static int overlaps(const flock_entry_t *e, uint64_t start, uint64_t end) {
    return e->start < end && start < e->end;
}

int flock_count(void) {
    int n = 0;
    for (int i = 0; i < FLOCK_MAX; i++) {
        n += table[i].used;
    }
    return n;
}

static int free_entries(void) {
    return FLOCK_MAX - flock_count();
}

static flock_entry_t *claim(void) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        if (!table[i].used) {
            table[i].used = 1;
            return &table[i];
        }
    }
    return (flock_entry_t *)0;
}

static const flock_entry_t *conflict(uint32_t ino, int pid, int type,
                                     uint64_t start, uint64_t end) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        const flock_entry_t *e = &table[i];
        if (!e->used || e->ino != ino || e->pid == pid) {
            continue;
        }
        if (!overlaps(e, start, end)) {
            continue;
        }
        if (type == OS_FLOCK_RD && e->type == OS_FLOCK_RD) {
            continue;
        }
        return e;
    }
    return (const flock_entry_t *)0;
}

int flock_test(uint32_t ino, int pid, int type, uint64_t start, uint64_t len,
               os_flock_t *out) {
    uint64_t end = range_end(start, len);
    const flock_entry_t *e = conflict(ino, pid, type, start, end);
    out->whence = 0;
    if (!e) {
        out->type = OS_FLOCK_UNLCK;
        out->start = 0;
        out->len = 0;
        out->pid = 0;
        return 0;
    }
    out->type = e->type;
    out->start = (int64_t)e->start;
    out->len = e->end == FLOCK_EOF ? 0 : (int64_t)(e->end - e->start);
    out->pid = e->pid;
    return 1;
}

int flock_set(uint32_t ino, int pid, int type, uint64_t start, uint64_t len) {
    uint64_t end = range_end(start, len);
    if (type != OS_FLOCK_UNLCK && conflict(ino, pid, type, start, end)) {
        return FLOCK_CONFLICT;
    }
    int needed = type != OS_FLOCK_UNLCK ? 1 : 0;
    for (int i = 0; i < FLOCK_MAX; i++) {
        const flock_entry_t *e = &table[i];
        if (e->used && e->ino == ino && e->pid == pid &&
            e->start < start && e->end > end) {
            needed++;
        }
    }
    if (free_entries() < needed) {
        return FLOCK_FULL;
    }
    for (int i = 0; i < FLOCK_MAX; i++) {
        flock_entry_t *e = &table[i];
        if (!e->used || e->ino != ino || e->pid != pid || !overlaps(e, start, end)) {
            continue;
        }
        if (e->start < start && e->end > end) {
            flock_entry_t *tail = claim();
            tail->type = e->type;
            tail->pid = pid;
            tail->ino = ino;
            tail->start = end;
            tail->end = e->end;
            e->end = start;
        } else if (e->start < start) {
            e->end = start;
        } else if (e->end > end) {
            e->start = end;
        } else {
            e->used = 0;
        }
    }
    if (type == OS_FLOCK_UNLCK) {
        return 0;
    }
    for (int i = 0; i < FLOCK_MAX; i++) {
        flock_entry_t *e = &table[i];
        if (!e->used || e->ino != ino || e->pid != pid || e->type != type) {
            continue;
        }
        if (e->end == start) {
            start = e->start;
            e->used = 0;
        } else if (e->start == end && end != FLOCK_EOF) {
            end = e->end;
            e->used = 0;
        }
    }
    flock_entry_t *n = claim();
    n->type = (uint8_t)type;
    n->pid = pid;
    n->ino = ino;
    n->start = start;
    n->end = end;
    return 0;
}

int flock_release_file(uint32_t ino, int pid) {
    int released = 0;
    for (int i = 0; i < FLOCK_MAX; i++) {
        if (table[i].used && table[i].ino == ino && table[i].pid == pid) {
            table[i].used = 0;
            released++;
        }
    }
    return released;
}

int flock_release_pid(int pid) {
    int released = 0;
    for (int i = 0; i < FLOCK_MAX; i++) {
        if (table[i].used && table[i].pid == pid) {
            table[i].used = 0;
            released++;
        }
    }
    return released;
}
