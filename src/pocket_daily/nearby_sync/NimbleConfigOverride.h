#pragma once

// Arduino-ESP32 supplies NimBLE defaults from sdkconfig.h. Include that file
// first and then narrow only the settings used by Pocket's peripheral-only
// control plane. A forced include makes the same values visible to both the
// application and NimBLE-Arduino translation units without editing a library.
#include <sdkconfig.h>

#undef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1

#undef CONFIG_BT_NIMBLE_MAX_BONDS
#define CONFIG_BT_NIMBLE_MAX_BONDS 2

#undef CONFIG_BT_NIMBLE_MAX_CCCDS
#define CONFIG_BT_NIMBLE_MAX_CCCDS 2

#undef CONFIG_BT_NIMBLE_ROLE_CENTRAL
#define CONFIG_BT_NIMBLE_ROLE_CENTRAL 0
#undef CONFIG_NIMBLE_ROLE_CENTRAL

#undef CONFIG_BT_NIMBLE_ROLE_OBSERVER
#define CONFIG_BT_NIMBLE_ROLE_OBSERVER 0
#undef CONFIG_NIMBLE_ROLE_OBSERVER

#undef CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU
#define CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU 247

#undef CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE
// Keep the upstream NimBLE-Arduino default.  A previous X3 optimization
// reduced this to 3072 bytes, but authenticated pairing then reset the device
// immediately after displaying its passkey.  Pairing performs substantially
// more host work than advertising, so this stack is not a safe place to reclaim
// the final kilobyte of RAM.
#define CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE 4096
