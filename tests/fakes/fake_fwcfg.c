/* tests/fakes/fake_fwcfg.c - Q11
 *
 * The QEMU firmware-config device, as a scripted answer to port reads.
 *
 * kernel/dev/fwcfg.c talks to two I/O ports and nothing else, which makes
 * it one of the easiest drivers in this tree to test off the machine and
 * one of the more important: it decides whether the boot self-tests run
 * at all. A silent failure in it turns the whole serial harness into a
 * passing no-op.
 *
 * This backs the `inb`/`outw` in tests/fakes/arch/x86_64/io.h. An item
 * nobody scripted reads back 0xFF, which is what an unclaimed port does
 * on real hardware and is exactly the case the no-device test wants. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

void panic(const char *msg);

#define MAX_ITEMS 8
#define MAX_BYTES 65536

typedef struct {
    uint16_t selector;
    uint8_t data[MAX_BYTES];
    uint32_t len;
    int used;
} item_t;

static item_t items[MAX_ITEMS];
static uint16_t selected;
static uint32_t pos;

void fake_fwcfg_reset(void);
void fake_fwcfg_set_item(uint16_t selector, const uint8_t *data, uint32_t len);
void fake_fwcfg_append_item(uint16_t selector, const uint8_t *data, uint32_t len);
uint8_t fake_port_inb(uint16_t port);
void fake_port_outw(uint16_t port, uint16_t value);

static item_t *find(uint16_t selector) {
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (items[i].used && items[i].selector == selector) {
            return &items[i];
        }
    }
    return NULL;
}

void fake_fwcfg_reset(void) {
    memset(items, 0, sizeof(items));
    selected = 0xFFFF;
    pos = 0;
}

void fake_fwcfg_set_item(uint16_t selector, const uint8_t *data, uint32_t len) {
    item_t *it = find(selector);
    if (!it) {
        for (int i = 0; i < MAX_ITEMS; i++) {
            if (!items[i].used) { it = &items[i]; break; }
        }
    }
    if (!it) {
        panic("fake_fwcfg: out of item slots");
    }
    if (len > MAX_BYTES) {
        panic("fake_fwcfg: item larger than the fake can hold");
    }
    it->used = 1;
    it->selector = selector;
    it->len = len;
    if (len) {
        memcpy(it->data, data, len);
    }
}

void fake_fwcfg_append_item(uint16_t selector, const uint8_t *data, uint32_t len) {
    item_t *it = find(selector);
    if (!it) {
        fake_fwcfg_set_item(selector, data, len);
        return;
    }
    if (it->len + len > MAX_BYTES) {
        panic("fake_fwcfg: item grew larger than the fake can hold");
    }
    memcpy(it->data + it->len, data, len);
    it->len += len;
}

/* ---- the port layer the io.h shadow calls ---------------------------- */

#define FWCFG_PORT_SEL  0x510
#define FWCFG_PORT_DATA 0x511

void fake_port_outw(uint16_t port, uint16_t value) {
    if (port == FWCFG_PORT_SEL) {
        selected = value;
        pos = 0;
    }
}

uint8_t fake_port_inb(uint16_t port) {
    if (port != FWCFG_PORT_DATA) {
        return 0xFF;
    }
    item_t *it = find(selected);
    if (!it || pos >= it->len) {
        /* Past the end of an item, or an item nobody scripted. An
         * unclaimed port reads 0xFF, and a device that has run out of
         * bytes keeps returning them - both are what the real hardware
         * does and both are cases fwcfg.c has to survive. */
        return 0xFF;
    }
    return it->data[pos++];
}
