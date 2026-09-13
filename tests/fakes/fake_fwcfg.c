#include <stdint.h>
#include <string.h>
#include <stdlib.h>

void panic(const char *message);

#define MAX_ITEMS 8
#define MAX_BYTES 65536

typedef struct {
    uint16_t selector;
    uint8_t data[MAX_BYTES];
    uint32_t length;
    int used;
} item_t;

static item_t items[MAX_ITEMS];
static uint16_t selected;
static uint32_t position;

void fake_fwcfg_reset(void);
void fake_fwcfg_set_item(uint16_t selector, const uint8_t *data, uint32_t length);
void fake_fwcfg_append_item(uint16_t selector, const uint8_t *data, uint32_t length);
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
    position = 0;
}

void fake_fwcfg_set_item(uint16_t selector, const uint8_t *data, uint32_t length) {
    item_t *it = find(selector);
    if (!it) {
        for (int i = 0; i < MAX_ITEMS; i++) {
            if (!items[i].used) { it = &items[i]; break; }
        }
    }
    if (!it) {
        panic("fake_fwcfg: out of item slots");
    }
    if (length > MAX_BYTES) {
        panic("fake_fwcfg: item larger than the fake can hold");
    }
    it->used = 1;
    it->selector = selector;
    it->length = length;
    if (length) {
        memcpy(it->data, data, length);
    }
}

void fake_fwcfg_append_item(uint16_t selector, const uint8_t *data, uint32_t length) {
    item_t *it = find(selector);
    if (!it) {
        fake_fwcfg_set_item(selector, data, length);
        return;
    }
    if (it->length + length > MAX_BYTES) {
        panic("fake_fwcfg: item grew larger than the fake can hold");
    }
    memcpy(it->data + it->length, data, length);
    it->length += length;
}

#define FWCFG_PORT_SEL  0x510
#define FWCFG_PORT_DATA 0x511

void fake_port_outw(uint16_t port, uint16_t value) {
    if (port == FWCFG_PORT_SEL) {
        selected = value;
        position = 0;
    }
}

uint8_t fake_port_inb(uint16_t port) {
    if (port != FWCFG_PORT_DATA) {
        return 0xFF;
    }
    item_t *it = find(selected);
    if (!it || position >= it->length) {
        return 0xFF;
    }
    return it->data[position++];
}
