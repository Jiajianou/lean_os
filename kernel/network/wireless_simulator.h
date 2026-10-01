#pragma once

#include <stdint.h>

#include "network/wireless_manager.h"

/* A simulated radio and five access points behind it, for machines with no
   wireless card - see wireless_simulator.c for what it is and is not. */

/* Spawns the simulated radio's task when fw_cfg asks for one
   (opt/leanos/wireless=simulated). Returns whether it did. */
int wireless_simulator_start(void);

/* The backend, freshly reset - for the host tests, which run the manager
   over it without a task. */
const wireless_backend_t *wireless_simulator_backend(void);

/* How many keys the station installed that the access point did not derive,
   and whether the station is through the handshake with both keys in. */
uint32_t wireless_simulator_keys_wrong(void);
int wireless_simulator_authorized(void);
