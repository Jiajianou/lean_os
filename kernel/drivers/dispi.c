#include "dispi.h"

#include "architecture/x86_64/io.h"
#include "kernel_log.h"

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

#define DISPI_ID0 0xB0C0
#define DISPI_ID5 0xB0C5

#define DISPI_DISABLED    0x00
#define DISPI_ENABLED     0x01
#define DISPI_LFB_ENABLED 0x40

#define DISPI_MAX_XRES 4096
#define DISPI_MAX_YRES 2560

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

    uint32_t blocks = dispi_read(DISPI_INDEX_VIDEO_MEMORY_64K);
    vram_bytes = blocks ? blocks * 64u * 1024u : 4u * 1024u * 1024u;

    for (int i = 0; i < CANDIDATE_COUNT && mode_count < DISPLAY_MAX_MODES; i++) {
        uint32_t w = CANDIDATES[i].width;
        uint32_t h = CANDIDATES[i].height;
        if (w > DISPI_MAX_XRES || h > DISPI_MAX_YRES) {
            continue;
        }
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

    dispi_write(DISPI_INDEX_ENABLE, DISPI_DISABLED);
    dispi_write(DISPI_INDEX_XRES, (uint16_t)w);
    dispi_write(DISPI_INDEX_YRES, (uint16_t)h);
    dispi_write(DISPI_INDEX_BPP, 32);
    dispi_write(DISPI_INDEX_BANK, 0);
    dispi_write(DISPI_INDEX_VIRT_WIDTH, (uint16_t)w);
    dispi_write(DISPI_INDEX_X_OFFSET, 0);
    dispi_write(DISPI_INDEX_Y_OFFSET, 0);
    dispi_write(DISPI_INDEX_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);

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
