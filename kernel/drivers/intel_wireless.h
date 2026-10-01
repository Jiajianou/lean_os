#pragma once

#include <stdint.h>

#include "network/ieee80211.h"

/* Intel's AX201 and the rest of its 22000 family: find the device, start
   Intel's firmware on it, and scan. The radio is driven from a kernel task of
   its own, so a firmware that takes its time - or never answers - costs that
   task and nothing else. */

#define INTEL_WIRELESS_MAX_NETWORKS 48

typedef struct {
    ieee80211_network_t network;
    int8_t signal_dbm;
    uint8_t band;
    uint32_t seen;
    /* The device's clock when this network's last beacon arrived, beside the
       beacon's own timestamp - what an association tells the firmware so it
       knows when to wake for the next one. */
    uint32_t sync_device_time;
} intel_wireless_network_t;

enum {
    INTEL_WIRELESS_STATE_ABSENT = 0,
    INTEL_WIRELESS_STATE_STARTING,
    INTEL_WIRELESS_STATE_READY,
    INTEL_WIRELESS_STATE_SCANNING,
    INTEL_WIRELESS_STATE_FAILED,
    INTEL_WIRELESS_STATE_RADIO_OFF,
};

/* Spawns the driver's task when the machine has one of these devices, and
   says whether it did. */
int intel_wireless_start(void);

int intel_wireless_state(void);

/* A copy of what the last scan found, strongest first. */
uint32_t intel_wireless_networks(intel_wireless_network_t *out, uint32_t capacity);

/* The device's own address, from its NVM; 0 until the firmware has said. */
int intel_wireless_mac_address(uint8_t out[6]);

/* Asks the driver's task for a new scan; it runs the next time the task
   looks, and intel_wireless_networks() has the result when the state is
   READY again. */
void intel_wireless_request_scan(void);

/* For the host tests: the steps the task takes, without the task. */
int intel_wireless_bring_up(volatile uint8_t *registers, uint32_t pci_device_id, const uint8_t *firmware_file,
                            uint32_t firmware_length);
int intel_wireless_scan(void);

/* The radio as the wireless manager drives it - network/wireless_manager.h. */
#include "network/wireless_manager.h"
const wireless_backend_t *intel_wireless_backend(void);
