#include "flock.h"

#include "library/spinlock.h"

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

/* M225: the table had no lock at all. Every entry point decides from what
   it reads (conflict() finds no holder, claim() finds a free entry) and then
   writes, and fcntl(F_SETLKW)'s waiters are woken together by every unlock
   (FLOCK_CHAN) - so on more than one processor two of them retried at the
   same moment, both found the range free, and both were told they held the
   write lock; or two claims took the same entry and one lock vanished. A
   thread exiting (flock_release_pid) cleared entries under a third's scan.
   Nothing in here takes another lock or sleeps, so this is a leaf. */
static spinlock_t flock_lock;

static uint64_t range_end(uint64_t start, uint64_t length) {
    if (length == 0 || start > FLOCK_EOF - length) {
        return FLOCK_EOF;
    }
    return start + length;
}

static int overlaps(const flock_entry_t *e, uint64_t start, uint64_t end) {
    return e->start < end && start < e->end;
}

static int count_locked(void) {
    int n = 0;
    for (int i = 0; i < FLOCK_MAX; i++) {
        n += table[i].used;
    }
    return n;
}

int flock_count(void) {
    uint64_t flags = spin_lock_irqsave(&flock_lock);
    int n = count_locked();
    spin_unlock_irqrestore(&flock_lock, flags);
    return n;
}

static int free_entries(void) {
    return FLOCK_MAX - count_locked();
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

int flock_test(uint32_t ino, int pid, int type, uint64_t start, uint64_t length,
               os_flock_t *out) {
    uint64_t end = range_end(start, length);
    os_flock_t answer;
    answer.whence = 0;
    uint64_t flags = spin_lock_irqsave(&flock_lock);
    const flock_entry_t *e = conflict(ino, pid, type, start, end);
    if (e) {
        answer.type = e->type;
        answer.start = (int64_t)e->start;
        answer.length = e->end == FLOCK_EOF ? 0 : (int64_t)(e->end - e->start);
        answer.pid = e->pid;
    } else {
        answer.type = OS_FLOCK_UNLCK;
        answer.start = 0;
        answer.length = 0;
        answer.pid = 0;
    }
    spin_unlock_irqrestore(&flock_lock, flags);
    *out = answer;
    return e ? 1 : 0;
}

static int set_locked(uint32_t ino, int pid, int type, uint64_t start, uint64_t length);

int flock_set(uint32_t ino, int pid, int type, uint64_t start, uint64_t length) {
    uint64_t flags = spin_lock_irqsave(&flock_lock);
    int result = set_locked(ino, pid, type, start, length);
    spin_unlock_irqrestore(&flock_lock, flags);
    return result;
}

static int set_locked(uint32_t ino, int pid, int type, uint64_t start, uint64_t length) {
    uint64_t end = range_end(start, length);
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
    uint64_t flags = spin_lock_irqsave(&flock_lock);
    for (int i = 0; i < FLOCK_MAX; i++) {
        if (table[i].used && table[i].ino == ino && table[i].pid == pid) {
            table[i].used = 0;
            released++;
        }
    }
    spin_unlock_irqrestore(&flock_lock, flags);
    return released;
}

int flock_release_pid(int pid) {
    int released = 0;
    uint64_t flags = spin_lock_irqsave(&flock_lock);
    for (int i = 0; i < FLOCK_MAX; i++) {
        if (table[i].used && table[i].pid == pid) {
            table[i].used = 0;
            released++;
        }
    }
    spin_unlock_irqrestore(&flock_lock, flags);
    return released;
}
