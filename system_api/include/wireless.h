#pragma once

#include <stdint.h>

/* The wireless network, as a program sees it: what the radio is doing, what
   it last heard, and a way to ask it to join one. One system call carries
   all of it (SYS_wireless, an operation and two arguments), and every
   operation but STATUS needs CAP_NETWORK - a list of the networks around a
   machine says where the machine is.

   The kernel keeps the networks it has joined in /etc/wireless/known and
   joins the strongest of them by itself at boot; a program never sees a
   password again once it has handed one over. */

#define WIRELESS_OPERATION_STATUS     0
#define WIRELESS_OPERATION_SCAN       1
#define WIRELESS_OPERATION_NETWORKS   2
#define WIRELESS_OPERATION_CONNECT    3
#define WIRELESS_OPERATION_DISCONNECT 4
#define WIRELESS_OPERATION_FORGET     5

#define WIRELESS_STATE_ABSENT        0
#define WIRELESS_STATE_STARTING      1
#define WIRELESS_STATE_RADIO_OFF     2
#define WIRELESS_STATE_IDLE          3
#define WIRELESS_STATE_SCANNING      4
#define WIRELESS_STATE_JOINING       5
#define WIRELESS_STATE_SECURING      6
#define WIRELESS_STATE_ADDRESSING    7
#define WIRELESS_STATE_CONNECTED     8
#define WIRELESS_STATE_FAILED        9

#define WIRELESS_ERROR_NONE                 0
#define WIRELESS_ERROR_NOT_FOUND            1
#define WIRELESS_ERROR_REJECTED             2
#define WIRELESS_ERROR_WRONG_PASSWORD       3
#define WIRELESS_ERROR_TIMED_OUT            4
#define WIRELESS_ERROR_NO_ADDRESS           5
#define WIRELESS_ERROR_UNSUPPORTED_SECURITY 6
#define WIRELESS_ERROR_DEVICE               7
#define WIRELESS_ERROR_BAD_PASSWORD_FORMAT  8
#define WIRELESS_ERROR_DISCONNECTED         9

#define WIRELESS_SECURITY_OPEN       0x01u
#define WIRELESS_SECURITY_WEP        0x02u
#define WIRELESS_SECURITY_WPA_PSK    0x04u
#define WIRELESS_SECURITY_WPA2_PSK   0x08u
#define WIRELESS_SECURITY_WPA3_SAE   0x10u
#define WIRELESS_SECURITY_ENTERPRISE 0x20u

#define WIRELESS_SSID_MAX 32
#define WIRELESS_PASSWORD_MAX 64
#define WIRELESS_NETWORKS_MAX 48

typedef struct {
    char ssid[WIRELESS_SSID_MAX + 1];
    uint8_t ssid_length;
    uint8_t bssid[6];
    int8_t signal_dbm;
    uint8_t channel;
    uint8_t security;
    uint8_t known;
    uint8_t connected;
    uint8_t supported;
} os_wireless_network_t;

typedef struct {
    int32_t state;
    int32_t error;
    char ssid[WIRELESS_SSID_MAX + 1];
    uint8_t ssid_length;
    uint8_t address[6];
    uint32_t ip;
    uint32_t scan_generation;
    int8_t signal_dbm;
    uint8_t channel;
    uint8_t reserved[2];
} os_wireless_status_t;

/* The password is the network's passphrase, 8 to 63 characters, or its key
   as 64 hexadecimal digits; empty for an open network, or for one this
   machine remembers. `remember` asks for the network to be kept once it has
   been joined - and rejoined by itself from then on. */
typedef struct {
    char ssid[WIRELESS_SSID_MAX + 1];
    uint8_t ssid_length;
    char password[WIRELESS_PASSWORD_MAX + 1];
    uint8_t remember;
} os_wireless_connect_t;
