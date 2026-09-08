/* kernel/fs/flock.c - M100: POSIX record locks. See flock.h. */
#include "flock.h"

typedef struct {
    uint8_t used;
    uint8_t type;   /* OS_FLOCK_RD or OS_FLOCK_WR */
    int pid;
    uint32_t ino;
    uint64_t start; /* inclusive */
    uint64_t end;   /* exclusive; FLOCK_EOF for "to end of file" */
} flock_entry_t;

/* An open-ended lock reaches every byte the file will ever have, so its
 * end is the largest offset there is. A lock over [x, FLOCK_EOF) then
 * conflicts with any lock at or past x, which is what "to EOF" means. */
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

/* The first lock held by somebody else that stands in the way of `type`
 * over [start, end). Two read locks never conflict; anything else
 * overlapping does. */
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
    out->whence = 0; /* SEEK_SET: the answer is absolute whatever the question was */
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
    /* Everything below changes the table, so the room has to be known
     * first: carving the range out of an own lock that strictly contains
     * it costs one entry (the lock becomes two), and recording the new
     * lock costs another. A process's own locks are disjoint, so at most
     * one of them can strictly contain the range. */
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
    /* 1. Carve [start, end) out of this process's own locks on the file,
     *    whatever their type: the new lock replaces them there. */
    for (int i = 0; i < FLOCK_MAX; i++) {
        flock_entry_t *e = &table[i];
        if (!e->used || e->ino != ino || e->pid != pid || !overlaps(e, start, end)) {
            continue;
        }
        if (e->start < start && e->end > end) {
            flock_entry_t *tail = claim(); /* counted above, so it is there */
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
    /* 2. Record the lock, absorbing any own lock of the same type that
     *    touches it, so that locking [0,10) and then [10,20) is one
     *    entry rather than two - the table is small and sqlite's
     *    lock/unlock pattern would otherwise fragment it. */
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
