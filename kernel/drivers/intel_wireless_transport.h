#pragma once

#include <stdint.h>

#include "drivers/intel_wireless_api.h"
#include "drivers/intel_wireless_firmware.h"

/* The PCIe half of an Intel 22000-family wireless device: its registers, the
   rings it reads commands from and writes notifications into, and the
   context-information block that hands it the firmware. The other half -
   what the commands mean - is intel_wireless.c.

   It is polled. The device's interrupt line is disabled at the PCI command
   register, so nothing fires; the cause register still latches, which is
   what this reads. */

#define INTEL_WIRELESS_RX_RING      256
#define INTEL_WIRELESS_RX_BUFFER    4096
#define INTEL_WIRELESS_COMMAND_SLOTS 32
#define INTEL_WIRELESS_COMMAND_QUEUE 0
#define INTEL_WIRELESS_TFD_QUEUE_SIZE_MAX 256
#define INTEL_WIRELESS_FIRST_TB_SIZE 20

enum {
    INTEL_WIRELESS_OK = 0,
    INTEL_WIRELESS_NOT_READY = -1,
    INTEL_WIRELESS_NO_CLOCK = -2,
    INTEL_WIRELESS_NO_MEMORY = -3,
    INTEL_WIRELESS_NO_ALIVE = -4,
    INTEL_WIRELESS_TIMED_OUT = -5,
    INTEL_WIRELESS_FIRMWARE_ERROR = -6,
    INTEL_WIRELESS_HARDWARE_ERROR = -7,
    INTEL_WIRELESS_QUEUE_FULL = -8,
    INTEL_WIRELESS_TOO_LARGE = -9,
    INTEL_WIRELESS_PERSISTENCE = -10,
    INTEL_WIRELESS_BAD_ALIVE = -11,
};

/* Called for every packet the device hands back that is not the reply to a
   command this side is waiting for - notifications, received frames, and
   replies nobody waits on. */
typedef void (*intel_wireless_packet_handler_t)(const intel_wireless_rx_packet_t *packet, const uint8_t *payload,
                                                uint32_t payload_length);

typedef struct {
    uint32_t rx_buffers_handled;
    uint32_t rx_packets;
    uint32_t commands_sent;
    uint32_t commands_answered;
    uint32_t interrupt_causes_seen;
    uint32_t last_interrupt_causes;
    uint32_t firmware_errors;
} intel_wireless_transport_statistics_t;

void intel_wireless_transport_attach(volatile uint8_t *registers);

uint32_t intel_wireless_read32(uint32_t offset);
void intel_wireless_write32(uint32_t offset, uint32_t value);
uint32_t intel_wireless_read_periphery(uint32_t address);
void intel_wireless_write_periphery(uint32_t address, uint32_t value);

int intel_wireless_prepare_card(void);
int intel_wireless_start_hardware(void);
int intel_wireless_start_firmware(const intel_wireless_firmware_t *firmware, uint32_t hardware_revision);
int intel_wireless_wait_alive(intel_wireless_alive_v6_t *alive, uint32_t timeout_ms);
void intel_wireless_firmware_alive(void);
void intel_wireless_stop(void);

int intel_wireless_radio_switch_off(void);

void intel_wireless_set_packet_handler(intel_wireless_packet_handler_t handler);

/* Sends one command and waits for the device's reply, which is copied into
   `response` (at most `capacity` bytes; its true length in `*response_length`).
   The reply's payload, not its header. */
int intel_wireless_send(uint8_t group, uint8_t command, uint8_t version, const void *payload, uint32_t length,
                        void *response, uint32_t capacity, uint32_t *response_length, uint32_t timeout_ms);

/* Drains the receive ring once: every waiting packet goes to the handler, and
   whatever the device has finished with is handed back to it. Returns a
   negative code when the device reported an error. */
int intel_wireless_poll(void);

const intel_wireless_transport_statistics_t *intel_wireless_transport_statistics(void);

/* The firmware's own record of what went wrong, read out of the device's
   memory: `count` words starting at `address`. */
int intel_wireless_read_device_memory(uint32_t address, uint32_t *out, uint32_t count);

void intel_wireless_delay_us(uint32_t microseconds);
void intel_wireless_sleep_ms(uint32_t milliseconds);
