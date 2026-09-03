#include "ac97.h"

#include "arch/x86_64/io.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "klog.h"
#include "lib/libk.h"
#include "mm/pmm.h"
#include "pci.h"

/* Intel 82801AA AC'97 audio - what QEMU's `-device AC97` presents. */
#define AC97_VENDOR_ID 0x8086
#define AC97_DEVICE_ID 0x2415

/* ---- BAR0: the mixer (codec registers) ---- */
#define MIXER_RESET        0x00
#define MIXER_MASTER_VOL   0x02
#define MIXER_PCM_OUT_VOL  0x18
#define MIXER_MUTE         0x8000

/* ---- BAR1: the bus-master block ---- */
/* PCM out ("PO") occupies 0x10..0x1B; the same layout repeats for PCM in
 * and the mic, neither of which this driver touches. */
#define PO_BDBAR 0x10 /* buffer descriptor list base address (32-bit physical) */
#define PO_CIV   0x14 /* current index value */
#define PO_LVI   0x15 /* last valid index */
#define PO_SR    0x16 /* status */
#define PO_PICB  0x18 /* position in current buffer */
#define PO_CR    0x1B /* control */
#define GLOB_CNT 0x2C /* global control */
#define GLOB_STA 0x30 /* global status */

#define CR_RPBM  0x01 /* run/pause bus master */
#define CR_RR    0x02 /* reset registers */
#define CR_IOCE  0x10 /* interrupt on completion enable */

#define SR_DCH   0x01 /* DMA controller halted */
#define SR_LVBCI 0x04 /* last valid buffer completion interrupt */
#define SR_BCIS  0x08 /* buffer completion interrupt status */
#define SR_FIFOE 0x10 /* FIFO error */

#define GLOB_CNT_COLD_RESET 0x02

/* The descriptor list the device walks. Each entry is a physical address
 * and a sample count; 32 entries is the hardware's fixed ring size. */
#define BDL_ENTRIES 32
#define BDL_IOC (1u << 31) /* raise an interrupt when this buffer is done */

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint16_t samples; /* *samples*, not frames - a stereo pair is two */
    uint16_t control;
} bdl_entry_t;

/* One page of descriptors and one contiguous page-aligned PCM buffer.
 * Fixed and physically contiguous for the same reason rtl8139.c's rings
 * are: the device is handed a physical base address and walks it itself,
 * with no scatter list and no allocator to consult. */
#define PCM_FRAMES 4096                        /* 4096 stereo frames = ~85 ms at 48 kHz */
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
    /* Status bits are write-1-to-clear, and writing back exactly what
     * was read acknowledges precisely the ones that were set - anything
     * else either clears a bit that arrived between the read and the
     * write, or fails to clear one that did. */
    outw((uint16_t)(bm_base + PO_SR), sr);
}

void ac97_init(void) {
    pci_device_t dev;
    if (!pci_find_device(AC97_VENDOR_ID, AC97_DEVICE_ID, &dev)) {
        klog_puts("[ac97] no AC'97 audio device found - the PC speaker is the only sound this boot.\n");
        return;
    }
    pci_enable_device(&dev);
    mixer_base = pci_bar0_io_base(&dev);
    bm_base = pci_bar1_io_base(&dev);
    if (mixer_base == 0 || bm_base == 0) {
        /* Q16: a sound card this driver cannot address. A desktop with
         * no sound is a desktop; a machine that will not boot because
         * the sound card is memory-mapped is not. */
        klog_puts("[ac97] BARs are memory-mapped - carrying on without sound.\n");
        return;
    }

    /* Cold-reset the link, then the codec. Writing anything to the
     * mixer's reset register is the reset - the value is ignored. */
    outl((uint16_t)(bm_base + GLOB_CNT), GLOB_CNT_COLD_RESET);
    outw((uint16_t)(mixer_base + MIXER_RESET), 0);

    ac97_set_volume(100);

    /* One page for the descriptor list, and enough contiguous pages for
     * the PCM buffer. pmm_alloc_frame_dma hands out single frames, so the
     * buffer's pages are only contiguous because they are allocated back
     * to back from a fresh allocator - checked rather than assumed,
     * because a device walking a discontiguous buffer would produce
     * noise rather than an error.
     *
     * M90: _dma, not plain pmm_alloc_frame. Both addresses below are
     * written to the device as 32-bit values, and until this milestone
     * every frame in the machine was below 4 GiB so the truncation could
     * not happen. On a machine with more memory it can, and it would be a
     * silent one: the cast compiles, the driver reports success, and the
     * card DMAs into somebody else's page. */
    bdl_phys = pmm_alloc_frame_dma();
    bdl = (bdl_entry_t *)bdl_phys;
    k_memset(bdl, 0, 4096);

    pcm_phys = pmm_alloc_frame_dma();
    for (uint32_t i = 1; i < PCM_PAGES; i++) {
        uint64_t next = pmm_alloc_frame_dma();
        if (next != pcm_phys + (uint64_t)i * 4096) {
            klog_puts("[ac97] could not get a contiguous PCM buffer - audio disabled this boot.\n");
            return;
        }
    }
    pcm = (int16_t *)pcm_phys;
    k_memset(pcm, 0, PCM_BYTES);

    irq_register_handler(dev.irq_line, ac97_irq);
    pic_clear_mask(dev.irq_line);

    available = 1;
    klog_puts("[ac97] AC'97 audio at mixer 0x");
    klog_put_hex32(mixer_base);
    klog_puts(", bus master 0x");
    klog_put_hex32(bm_base);
    klog_puts(", IRQ 0x");
    klog_put_hex32(dev.irq_line);
    klog_puts(" - 16-bit stereo 48 kHz output ready.\n");
}

int ac97_available(void) {
    return available;
}

uint32_t ac97_max_frames(void) {
    return PCM_FRAMES;
}

/* Folds whatever the device's own status register shows into the count.
 *
 * This is here because the completion interrupt does not arrive, and the
 * investigation is worth recording rather than repeating: the device
 * asserts it (GLOB_STA's POINT bit is set), the PIC's mask registers show
 * IRQ 11 and the cascade both unmasked, and the PIC's *request* register
 * is nevertheless empty - so the interrupt never reaches the 8259 at all.
 * On this machine the firmware routes PCI interrupts through the I/O
 * APIC, which this kernel does not program; it drives the LAPIC for SMP
 * and nothing else. Wiring up an I/O APIC is a subsystem, not a bullet on
 * an audio milestone.
 *
 * So this polls, exactly as rtl8139.c already does for its own transmit
 * completion - and the IRQ handler stays registered because it is correct
 * and costs nothing on a machine that does deliver it. A count that can
 * be reached both ways is not a compromise here: it is monotonic either
 * way, and "how many buffers has the device finished" is the question
 * every caller actually has. */
static void ac97_poll(void) {
    if (!available) {
        return;
    }
    uint16_t sr = inw((uint16_t)(bm_base + PO_SR));
    uint16_t done = (uint16_t)(sr & (SR_BCIS | SR_LVBCI));
    if (done) {
        completions++;
        outw((uint16_t)(bm_base + PO_SR), done); /* write-1-to-clear, and only what was seen */
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
    /* AC'97 volume registers are *attenuation*: 0 is loudest and 31 (six
     * bits, per channel) is quietest, with bit 15 an explicit mute. So a
     * percentage has to be inverted, and 0 gets the mute bit rather than
     * merely maximum attenuation - "muted" and "very quiet" are
     * different claims and only one of them is what a mute switch
     * promises. */
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
    /* Stop whatever is running and clear the ring's own state before
     * re-arming it. There is one stream here, so a second play replaces
     * the first rather than queueing behind it - the same "there is one
     * speaker" rule pcspk.h states, applied to the device that could in
     * principle do better and deliberately does not yet. */
    outb((uint16_t)(bm_base + PO_CR), 0);
    outb((uint16_t)(bm_base + PO_CR), CR_RR);
    while (inb((uint16_t)(bm_base + PO_CR)) & CR_RR) {
        /* the reset bit clears itself when the block is idle */
    }

    k_memcpy(pcm, samples, (size_t)frames * 2 * sizeof(int16_t));

    bdl[0].addr = (uint32_t)pcm_phys;
    bdl[0].samples = (uint16_t)(frames * 2); /* stereo: two samples per frame */
    bdl[0].control = (uint16_t)(BDL_IOC >> 16);

    outl((uint16_t)(bm_base + PO_BDBAR), (uint32_t)bdl_phys);
    outb((uint16_t)(bm_base + PO_LVI), 0); /* one descriptor, so the last valid index is the first */
    outb((uint16_t)(bm_base + PO_CR), CR_RPBM | CR_IOCE);
    return 0;
}

void ac97_debug_dump(void) {
    if (!available) {
        klog_puts("[ac97] no device\n");
        return;
    }
    klog_puts("[ac97] SR=0x");
    klog_put_hex32(inw((uint16_t)(bm_base + PO_SR)));
    klog_puts(" CIV=0x");
    klog_put_hex32(inb((uint16_t)(bm_base + PO_CIV)));
    klog_puts(" LVI=0x");
    klog_put_hex32(inb((uint16_t)(bm_base + PO_LVI)));
    klog_puts(" PICB=0x");
    klog_put_hex32(inw((uint16_t)(bm_base + PO_PICB)));
    klog_puts(" CR=0x");
    klog_put_hex32(inb((uint16_t)(bm_base + PO_CR)));
    klog_puts(" GLOB_STA=0x");
    klog_put_hex32(inl((uint16_t)(bm_base + GLOB_STA)));
    klog_putc('\n');
}
