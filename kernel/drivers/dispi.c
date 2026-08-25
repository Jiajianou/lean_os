#include "dispi.h"

#include "arch/x86_64/io.h"
#include "klog.h"

/* The register file, at the two ports every Bochs-derived adapter has
 * exposed since the original bochs-vbe: write an index to 0x1CE, then
 * read or write the 16-bit value at 0x1CF. */
#define DISPI_IOPORT_INDEX 0x01CE
#define DISPI_IOPORT_DATA  0x01CF

#define DISPI_INDEX_ID               0
#define DISPI_INDEX_XRES             1
#define DISPI_INDEX_YRES             2
#define DISPI_INDEX_BPP              3
#define DISPI_INDEX_ENABLE           4
#define DISPI_INDEX_BANK             5
#define DISPI_INDEX_VIRT_WIDTH       6
#define DISPI_INDEX_VIRT_HEIGHT      7
#define DISPI_INDEX_X_OFFSET         8
#define DISPI_INDEX_Y_OFFSET         9
#define DISPI_INDEX_VIDEO_MEMORY_64K 10

/* The interface has been revised five times; every revision answers with
 * its own id and all of them accept the subset used here. Accepting a
 * range rather than one exact value is what keeps this working against
 * whichever QEMU is installed. */
#define DISPI_ID0 0xB0C0
#define DISPI_ID5 0xB0C5

#define DISPI_DISABLED    0x00
#define DISPI_ENABLED     0x01
#define DISPI_LFB_ENABLED 0x40

/* The interface's own hard ceilings. A geometry past either is refused by
 * the device, so it is refused here first. */
#define DISPI_MAX_XRES 4096
#define DISPI_MAX_YRES 2560

/* Every mode this driver is willing to offer, before validation. Standard
 * sizes only, and deliberately including nothing exotic: the list is a
 * judgement about what a person would pick, and the validation below is
 * what keeps that judgement from including something the device would
 * refuse. 1024x768 is boot.c's own preference and is always in the list
 * for that reason - it is the one mode known to have worked at least
 * once on this machine. */
static const display_mode_t CANDIDATES[] = {
    {  800,  600 },
    { 1024,  768 },
    { 1152,  864 },
    { 1280,  720 },
    { 1280, 1024 },
    { 1440,  900 },
    { 1600,  900 },
    { 1680, 1050 },
    { 1920, 1080 },
};

#define CANDIDATE_COUNT ((int)(sizeof(CANDIDATES) / sizeof(CANDIDATES[0])))

static int available;
static uint32_t vram_bytes;
static display_mode_t modes[DISPLAY_MAX_MODES];
static int mode_count;

static void dispi_write(uint16_t index, uint16_t value) {
    outw(DISPI_IOPORT_INDEX, index);
    outw(DISPI_IOPORT_DATA, value);
}

static uint16_t dispi_read(uint16_t index) {
    outw(DISPI_IOPORT_INDEX, index);
    return inw(DISPI_IOPORT_DATA);
}

void dispi_init(void) {
    available = 0;
    mode_count = 0;
    vram_bytes = 0;

    uint16_t id = dispi_read(DISPI_INDEX_ID);
    if (id < DISPI_ID0 || id > DISPI_ID5) {
        klog_puts("[dispi] no Bochs/QEMU DISPI adapter (id register read 0x");
        klog_put_hex32(id);
        klog_puts(") - resolution stays whatever the firmware chose.\n");
        return;
    }
    available = 1;

    /* Read from the device rather than assumed: QEMU's stdvga defaults to
     * 16 MiB but is a command-line knob, and the amount of video memory
     * is the real ceiling on which modes can be offered. A revision old
     * enough not to have this register reports 0, in which case the
     * conservative 4 MiB floor below still admits 1024x768. */
    uint32_t blocks = dispi_read(DISPI_INDEX_VIDEO_MEMORY_64K);
    vram_bytes = blocks ? blocks * 64u * 1024u : 4u * 1024u * 1024u;

    for (int i = 0; i < CANDIDATE_COUNT && mode_count < DISPLAY_MAX_MODES; i++) {
        uint32_t w = CANDIDATES[i].width;
        uint32_t h = CANDIDATES[i].height;
        if (w > DISPI_MAX_XRES || h > DISPI_MAX_YRES) {
            continue;
        }
        /* width * height * 4 is the smallest this mode could possibly
         * need. The device may pick a larger stride than width * 4 (see
         * dispi_set_mode's read-back), which only makes the real figure
         * bigger - so a mode that fails this check cannot fit either way,
         * and one that passes is checked again for real once the device
         * has told us the stride it chose. */
        uint64_t need = (uint64_t)w * h * 4u;
        if (need > vram_bytes) {
            continue;
        }
        modes[mode_count++] = CANDIDATES[i];
    }

    klog_puts("[dispi] Bochs/QEMU DISPI adapter id 0x");
    klog_put_hex32(id);
    klog_puts(", 0x");
    klog_put_hex32(vram_bytes);
    klog_puts(" bytes of video memory, 0x");
    klog_put_hex32((uint32_t)mode_count);
    klog_puts(" modes offered.\n");
}

int dispi_available(void) {
    return available;
}

uint32_t dispi_vram_bytes(void) {
    return vram_bytes;
}

int dispi_mode_count(void) {
    return mode_count;
}

int dispi_get_modes(display_mode_t *out, int max) {
    int n = mode_count < max ? mode_count : max;
    for (int i = 0; i < n; i++) {
        out[i] = modes[i];
    }
    return mode_count;
}

int dispi_set_mode(uint32_t w, uint32_t h, uint32_t *out_pitch) {
    if (!available) {
        return -1;
    }
    int offered = 0;
    for (int i = 0; i < mode_count; i++) {
        if (modes[i].width == w && modes[i].height == h) {
            offered = 1;
            break;
        }
    }
    if (!offered) {
        return -1;
    }

    /* The DISPI programming sequence, and it is a sequence: geometry is
     * only latched while the adapter is disabled, so disable, write, then
     * re-enable with the linear-framebuffer bit set. Enabling without
     * DISPI_LFB_ENABLED leaves the device in banked mode, where the
     * framebuffer this whole OS draws through would be a 64 KiB window
     * rather than the flat region fb.c maps. */
    dispi_write(DISPI_INDEX_ENABLE, DISPI_DISABLED);
    dispi_write(DISPI_INDEX_XRES, (uint16_t)w);
    dispi_write(DISPI_INDEX_YRES, (uint16_t)h);
    dispi_write(DISPI_INDEX_BPP, 32);
    dispi_write(DISPI_INDEX_BANK, 0);
    dispi_write(DISPI_INDEX_VIRT_WIDTH, (uint16_t)w);
    dispi_write(DISPI_INDEX_X_OFFSET, 0);
    dispi_write(DISPI_INDEX_Y_OFFSET, 0);
    dispi_write(DISPI_INDEX_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);

    /* Read back what the device actually did, all three of it. Asking for
     * a mode and assuming you got it is how a resolution setting becomes
     * a sheared screen with no way back: fb.h has documented since M16
     * that the pitch "is not necessarily width * 4", and VIRT_WIDTH is
     * where the device says what it chose. */
    uint32_t got_w = dispi_read(DISPI_INDEX_XRES);
    uint32_t got_h = dispi_read(DISPI_INDEX_YRES);
    uint32_t got_bpp = dispi_read(DISPI_INDEX_BPP);
    uint32_t virt_w = dispi_read(DISPI_INDEX_VIRT_WIDTH);
    if (got_w != w || got_h != h || got_bpp != 32 || virt_w < w) {
        return -1;
    }
    uint32_t pitch = virt_w * 4u;
    if ((uint64_t)pitch * h > vram_bytes) {
        return -1;
    }
    *out_pitch = pitch;
    return 0;
}
