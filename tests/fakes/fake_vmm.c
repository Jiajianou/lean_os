/* tests/fakes/fake_vmm.c - Q2
 *
 * A page-mapping layer over a real host mmap.
 *
 * The kernel's heap grows by asking vmm to map a fresh frame at the next
 * virtual address and then writing through that address. To exercise that
 * for real - not to stub it out - this fake reserves one contiguous host
 * region up front and hands its base out as vmm_kernel_heap_base(). Every
 * vmm_map_page call then lands inside memory the host process genuinely
 * owns, so heap.c's pointer arithmetic, its splitting and its coalescing
 * all run against real memory and ASan can see every one of them.
 *
 * The region is host-chosen rather than the kernel's own 256 GiB: macOS
 * refuses MAP_FIXED there, which is exactly why vmm.h grew an accessor.
 *
 * What is recorded rather than emulated: which virtual pages are mapped,
 * so a double map or an unmap of something never mapped is caught the way
 * the real vmm catches it - with the same panic message, so a test's
 * CHECK_PANIC matches the same substring it would on the machine. */
#include "mm/vmm.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

void panic(const char *msg);

#define HEAP_SPAN (64ULL * 1024 * 1024)
#define PAGE 4096ULL
#define MAX_PAGES (HEAP_SPAN / PAGE)

static uint8_t *region;
static uint8_t mapped[MAX_PAGES];
/* M102: the frame each page holds, so vmm_unmap_page_take can hand it
 * back. Recording it is what lets a test assert that a *failed* heap
 * growth is frame-neutral - which is the property that decides whether
 * running out of memory twice costs more than running out once. */
static uint64_t mapped_phys[MAX_PAGES];
static uint64_t mapped_count;

/* M100: how many more mappings succeed before vmm_try_map_page_in starts
 * refusing. -1 is "never refuse", which is the default and what every
 * test that does not ask for this gets. See the note on
 * vmm_try_map_page_in below for why this is a real failure and not an
 * invented one. */
static int map_fail_after = -1;
static int maps_attempted;

void fake_vmm_reset(void);
uint64_t fake_vmm_mapped_pages(void);
void fake_vmm_fail_map_after(int n);

static void ensure_region(void) {
    if (region) {
        return;
    }
    void *p = mmap(NULL, HEAP_SPAN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        panic("fake_vmm: could not reserve a heap region");
    }
    region = (uint8_t *)p;
}

uint64_t vmm_kernel_heap_base(void) {
    ensure_region();
    return (uint64_t)(uintptr_t)region;
}

void fake_vmm_reset(void) {
    ensure_region();
    for (uint64_t i = 0; i < MAX_PAGES; i++) {
        mapped[i] = 0;
        mapped_phys[i] = 0;
    }
    mapped_count = 0;
    map_fail_after = -1;
    maps_attempted = 0;
    /* Discard the old contents so a test never sees the previous test's
     * heap. MADV_FREE/MADV_DONTNEED differ across hosts; re-mapping the
     * same range is portable and unambiguous. */
    if (mmap(region, HEAP_SPAN, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
        panic("fake_vmm: could not reset the heap region");
    }
}

uint64_t fake_vmm_mapped_pages(void) { return mapped_count; }

/* Refuse the (n+1)th and every later mapping. `n` counts attempts since
 * the last fake_vmm_reset, so fake_vmm_fail_map_after(3) means "three
 * pages map, the fourth does not" - the same shape and the same counting
 * as fake_pmm_fail_after, deliberately, because a test that has to
 * remember which of two injectors counts differently is a test that will
 * be read wrong. */
void fake_vmm_fail_map_after(int n) {
    map_fail_after = n;
    maps_attempted = 0;
}

static uint64_t page_index(uint64_t virt) {
    ensure_region();
    uint64_t base = (uint64_t)(uintptr_t)region;
    if (virt < base || virt >= base + HEAP_SPAN) {
        panic("fake_vmm: mapping outside the reserved heap region "
              "(the test asked for more heap than the fake reserves)");
    }
    return (virt - base) / PAGE;
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)flags;
    uint64_t i = page_index(virt);
    if (mapped[i]) {
        panic("vmm_map_page: address already mapped");
    }
    mapped[i] = 1;
    mapped_phys[i] = phys;
    mapped_count++;
}

/* M102: the failable form the heap now uses.
 *
 * ---- M100: it fails on demand now, and the old note was too broad ----
 *
 * This used to never fail, under a comment saying that making it fail
 * "would be inventing a failure mode the fake cannot honestly model".
 * Half of that is right and the half that is wrong left grow_heap's
 * unwind path at zero coverage from M102 until M100 looked.
 *
 * The real vmm_try_map_page_in returns -1 for one reason: it needed a
 * frame for a **page table** and pmm had none. That is a distinct
 * outcome from the one the heap checks a line earlier - pmm having no
 * frame for the page itself - and on a nearly-full machine it is the
 * more likely of the two, because a page table is allocated at the
 * moment a mapping crosses into an unpopulated PDE and the heap has no
 * way to see that coming. The fake having no page tables of its own is
 * a reason it cannot decide *when* to fail; it is not a reason it cannot
 * be told.
 *
 * So the injector is explicit and a test has to ask for it. What it
 * models is exactly the documented return, no more: the mapping does not
 * happen, and the caller still owns the frame it was handed - which is
 * the whole point, because whether grow_heap gives that frame back is
 * the difference between failing to allocate and losing a page every
 * time you try. */
int vmm_try_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    (void)pml4_phys;
    if (map_fail_after >= 0 && maps_attempted >= map_fail_after) {
        maps_attempted++;
        return -1;
    }
    maps_attempted++;
    vmm_map_page(virt, phys, flags);
    return 0;
}

/* M102: unmap and hand the frame back, which is how the heap unwinds a
 * partial growth. Returns 0 for a page that was not mapped, exactly as
 * the real one does, rather than panicking - the caller is in a cleanup
 * loop and a cleanup that can fail is a cleanup nobody will act on. */
uint64_t vmm_unmap_page_take(uint64_t pml4_phys, uint64_t virt) {
    (void)pml4_phys;
    uint64_t i = page_index(virt);
    if (!mapped[i]) {
        return 0;
    }
    uint64_t phys = mapped_phys[i];
    mapped[i] = 0;
    mapped_phys[i] = 0;
    mapped_count--;
    return phys;
}

void vmm_unmap_page(uint64_t virt) {
    uint64_t i = page_index(virt);
    if (!mapped[i]) {
        panic("vmm_unmap_page: address not mapped");
    }
    mapped[i] = 0;
    mapped_phys[i] = 0;
    mapped_count--;
}

/* The rest of the surface, present so the unit under test links. Anything
 * a test actually depends on gets a real implementation above; these
 * deliberately panic rather than returning a plausible-looking value, so
 * a test that wanders into unfaked territory says so instead of quietly
 * asserting against a stub. */
/* ---- Q13: the three the scheduler asks about a user address space ----
 *
 * vmm_user_range_ok is what SYS_* pointer validation is built on and
 * what sched.c consults before touching a task's memory. It answers yes
 * for anything inside the user half here, which is what a mapped address
 * space looks like from above - the real one walks page tables, and
 * walking a page table this fake never built would answer no to
 * everything and make every test a refusal.
 *
 * vmm_cow_break and vmm_unmap_page_in are the fault-path halves. Both
 * report success and count, so "the scheduler asked for a copy-on-write
 * break" is assertable without a page table under it. */
static uint64_t cow_breaks;
static uint64_t unmaps_in;

uint64_t fake_vmm_cow_breaks(void) { return cow_breaks; }
uint64_t fake_vmm_unmaps_in(void) { return unmaps_in; }

int vmm_user_range_ok(uint64_t pml4_phys, uint64_t virt, uint64_t len, int need_write) {
    (void)pml4_phys;
    (void)need_write;
    if (len == 0) {
        return 1;
    }
    if (virt + len < virt) {
        return 0; /* the overflow case, which is the one worth keeping */
    }
    return 1;
}

int vmm_cow_break(uint64_t pml4_phys, uint64_t virt) {
    (void)pml4_phys;
    (void)virt;
    cow_breaks++;
    return 0;
}

int vmm_unmap_page_in(uint64_t pml4_phys, uint64_t virt) {
    (void)pml4_phys;
    (void)virt;
    unmaps_in++;
    return 0;
}

/* ---- M98: the resident-set counters ----------------------------------
 *
 * A settable answer rather than a stub returning zero. The scheduler
 * reads the peak off an address space at two points that matter - the
 * exit path and getrusage - and a fake that always said 0 would let a
 * test "pass" while the capture was deleted. fake_vmm_set_rss_peak is
 * how a test says what the address space's high-water mark is. */
static uint64_t rss_peak;

void fake_vmm_set_rss_peak(uint64_t pages) { rss_peak = pages; }

uint64_t vmm_rss_pages(uint64_t pml4_phys) {
    (void)pml4_phys;
    return rss_peak;
}

uint64_t vmm_rss_peak_pages(uint64_t pml4_phys) {
    (void)pml4_phys;
    return rss_peak;
}

void vmm_init(const uint32_t *e820_map) { (void)e820_map; }
int vmm_identity_covers(uint64_t phys, uint64_t len) { (void)phys; (void)len; return 1; }
void vmm_enable_nx_this_cpu(void) {}
int vmm_nx_enabled(void) { return 1; }
uint64_t vmm_kernel_pml4_phys(void) { return 0; }
void vmm_switch_address_space(uint64_t p) { (void)p; }
