#include "ac97.h"

#include "architecture/x86_64/io.h"
#include "architecture/x86_64/interrupt_service_routines.h"
#include "architecture/x86_64/pic.h"
#include "kernel_log.h"
#include "library/kernel_library.h"
#include "memory_management/physical_memory.h"
#include "pci.h"

#define AC97_VENDOR_ID 0x8086
#define AC97_DEVICE_ID 0x2415

#define MIXER_RESET        0x00
#define MIXER_MASTER_VOL   0x02
#define MIXER_PCM_OUT_VOL  0x18
#define MIXER_MUTE         0x8000

#define PO_BDBAR 0x10
#define PO_CIV   0x14
#define PO_LVI   0x15
#define PO_SR    0x16
#define PO_PICB  0x18
#define PO_CR    0x1B
#define GLOB_CNT 0x2C
#define GLOB_STA 0x30

#define CR_RPBM  0x01
#define CR_RR    0x02
#define CR_IOCE  0x10

#define SR_DCH   0x01
#define SR_LVBCI 0x04
#define SR_BCIS  0x08
#define SR_FIFOE 0x10

#define GLOB_CNT_COLD_RESET 0x02

#define BDL_ENTRIES 32
#define BDL_IOC (1u << 31)

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint16_t samples;
    uint16_t control;
} bdl_entry_t;

#define PCM_FRAMES 4096
#define PCM_BYTES  (PCM_FRAMES * 2 * 2)
#define PCM_PAGES  ((PCM_BYTES + 4095) / 4096)

static uint16_t mixer_base;
static uint16_t bm_base;
static int available;
static volatile uint32_t completions;
static bdl_entry_t *bdl;
static int16_t *pcm;
static uint64_t bdl_phys;
static uint64_t pcm_phys;

static void ac97_irq(isr_regs_t *regs) {
    (void)regs;
    uint16_t sr = inw((uint16_t)(bm_base + PO_SR));
    if (sr & (SR_BCIS | SR_LVBCI)) {
        completions++;
    }
    outw((uint16_t)(bm_base + PO_SR), sr);
}

void ac97_init(void) {
    pci_device_t dev;
    if (!pci_find_device(AC97_VENDOR_ID, AC97_DEVICE_ID, &dev)) {
        kernel_log_puts("[ac97] no AC'97 audio device found - the PC speaker is the only sound this boot.\n");
        return;
    }
    pci_enable_device(&dev);
    mixer_base = pci_bar0_io_base(&dev);
    bm_base = pci_bar1_io_base(&dev);
    if (mixer_base == 0 || bm_base == 0) {
        kernel_log_puts("[ac97] BARs are memory-mapped - carrying on without sound.\n");
        return;
    }

    outl((uint16_t)(bm_base + GLOB_CNT), GLOB_CNT_COLD_RESET);
    outw((uint16_t)(mixer_base + MIXER_RESET), 0);

    ac97_set_volume(100);

    bdl_phys = physical_memory_alloc_frame_dma();
    bdl = (bdl_entry_t *)bdl_phys;
    k_memset(bdl, 0, 4096);

    pcm_phys = physical_memory_alloc_frame_dma();
    for (uint32_t i = 1; i < PCM_PAGES; i++) {
        uint64_t next = physical_memory_alloc_frame_dma();
        if (next != pcm_phys + (uint64_t)i * 4096) {
            kernel_log_puts("[ac97] could not get a contiguous PCM buffer - audio disabled this boot.\n");
            return;
        }
    }
    pcm = (int16_t *)pcm_phys;
    k_memset(pcm, 0, PCM_BYTES);

    irq_register_handler(dev.irq_line, ac97_irq);
    pic_clear_mask(dev.irq_line);

    available = 1;
    kernel_log_puts("[ac97] AC'97 audio at mixer 0x");
    kernel_log_put_hex32(mixer_base);
    kernel_log_puts(", bus master 0x");
    kernel_log_put_hex32(bm_base);
    kernel_log_puts(", IRQ 0x");
    kernel_log_put_hex32(dev.irq_line);
    kernel_log_puts(" - 16-bit stereo 48 kHz output ready.\n");
}

int ac97_available(void) {
    return available;
}

uint32_t ac97_max_frames(void) {
    return PCM_FRAMES;
}

static void ac97_poll(void) {
    if (!available) {
        return;
    }
    uint16_t sr = inw((uint16_t)(bm_base + PO_SR));
    uint16_t done = (uint16_t)(sr & (SR_BCIS | SR_LVBCI));
    if (done) {
        completions++;
        outw((uint16_t)(bm_base + PO_SR), done);
    }
}

uint32_t ac97_completions(void) {
    ac97_poll();
    return completions;
}

void ac97_set_volume(uint32_t percent) {
    if (!mixer_base) {
        return;
    }
    if (percent > 100) {
        percent = 100;
    }
    uint16_t value;
    if (percent == 0) {
        value = MIXER_MUTE;
    } else {
        uint16_t atten = (uint16_t)(31u - (percent * 31u / 100u));
        value = (uint16_t)((atten << 8) | atten);
    }
    outw((uint16_t)(mixer_base + MIXER_MASTER_VOL), value);
    outw((uint16_t)(mixer_base + MIXER_PCM_OUT_VOL), value);
}

int ac97_play(const int16_t *samples, uint32_t frames) {
    if (!available || frames == 0 || frames > PCM_FRAMES) {
        return -1;
    }
    outb((uint16_t)(bm_base + PO_CR), 0);
    outb((uint16_t)(bm_base + PO_CR), CR_RR);
    while (inb((uint16_t)(bm_base + PO_CR)) & CR_RR) {
    }

    k_memcpy(pcm, samples, (size_t)frames * 2 * sizeof(int16_t));

    bdl[0].addr = (uint32_t)pcm_phys;
    bdl[0].samples = (uint16_t)(frames * 2);
    bdl[0].control = (uint16_t)(BDL_IOC >> 16);

    outl((uint16_t)(bm_base + PO_BDBAR), (uint32_t)bdl_phys);
    outb((uint16_t)(bm_base + PO_LVI), 0);
    outb((uint16_t)(bm_base + PO_CR), CR_RPBM | CR_IOCE);
    return 0;
}

void ac97_debug_dump(void) {
    if (!available) {
        kernel_log_puts("[ac97] no device\n");
        return;
    }
    kernel_log_puts("[ac97] SR=0x");
    kernel_log_put_hex32(inw((uint16_t)(bm_base + PO_SR)));
    kernel_log_puts(" CIV=0x");
    kernel_log_put_hex32(inb((uint16_t)(bm_base + PO_CIV)));
    kernel_log_puts(" LVI=0x");
    kernel_log_put_hex32(inb((uint16_t)(bm_base + PO_LVI)));
    kernel_log_puts(" PICB=0x");
    kernel_log_put_hex32(inw((uint16_t)(bm_base + PO_PICB)));
    kernel_log_puts(" CR=0x");
    kernel_log_put_hex32(inb((uint16_t)(bm_base + PO_CR)));
    kernel_log_puts(" GLOB_STA=0x");
    kernel_log_put_hex32(inl((uint16_t)(bm_base + GLOB_STA)));
    kernel_log_putc('\n');
}
