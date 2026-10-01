#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "network/wireless_manager.h"

/* The kernel's side of the wireless manager, for the host tests: a file
   that lives in memory, an address that arrives when the test says, and a
   record of every frame handed to the IP stack. */

static char storage[4096];
static int storage_length = -1;
static uint32_t random_counter;
static int addressing_answer;
static int addressing_link_free = 1;
static uint32_t addressing_started;
static uint8_t delivered[16][1600];
static uint32_t delivered_length[16];
static uint32_t delivered_count;

void fake_wireless_glue_reset(int keep_storage) {
    if (!keep_storage) {
        storage_length = -1;
    }
    random_counter = 0;
    addressing_answer = 1;
    addressing_link_free = 1;
    addressing_started = 0;
    delivered_count = 0;
}

const char *fake_wireless_storage(int *length) {
    *length = storage_length;
    return storage;
}

void fake_wireless_set_storage(const char *text) {
    storage_length = (int)strlen(text);
    memcpy(storage, text, (size_t)storage_length);
}

void fake_wireless_set_addressing(int answer, int link_free) {
    addressing_answer = answer;
    addressing_link_free = link_free;
}

uint32_t fake_wireless_addressing_started(void) {
    return addressing_started;
}

const uint8_t *fake_wireless_delivered(uint32_t index, uint32_t *length) {
    if (index >= delivered_count) {
        return 0;
    }
    *length = delivered_length[index];
    return delivered[index];
}

uint32_t fake_wireless_delivered_count(void) {
    return delivered_count;
}

int wireless_storage_read(char *buffer, uint32_t capacity) {
    if (storage_length < 0 || (uint32_t)storage_length > capacity) {
        return -1;
    }
    memcpy(buffer, storage, (size_t)storage_length);
    return storage_length;
}

int wireless_storage_write(const char *buffer, uint32_t length) {
    if (length > sizeof(storage)) {
        return -1;
    }
    memcpy(storage, buffer, length);
    storage_length = (int)length;
    return 0;
}

void *wireless_allocate(uint32_t length) {
    return malloc(length);
}

void wireless_random(void *out, uint32_t length) {
    uint8_t *bytes = out;
    for (uint32_t i = 0; i < length; i++) {
        bytes[i] = (uint8_t)(random_counter++ * 37u + 11u);
    }
}

void wireless_deliver_ip(const uint8_t *frame, uint32_t length) {
    if (delivered_count < 16 && length <= 1600) {
        memcpy(delivered[delivered_count], frame, length);
        delivered_length[delivered_count++] = length;
    }
}

int wireless_addressing_start(const uint8_t address[6]) {
    (void)address;
    addressing_started++;
    return addressing_link_free;
}

int wireless_addressing_poll(uint32_t *ip) {
    if (addressing_answer > 0) {
        *ip = 0xC0A84D17u;
    }
    return addressing_answer;
}

void wireless_addressing_stop(void) {
}
