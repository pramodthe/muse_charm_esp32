// SPDX-License-Identifier: MIT
// Battery and power, from the board's AXP2101 power-management chip.
#pragma once

#include <stdbool.h>

typedef struct {
    bool known;     // the chip answered
    bool battery;   // a battery is connected
    int percent;    // its charge, 0..100 (only with a battery)
    bool charging;
    bool usb;       // USB power is present
} muse_power_t;

// Find the chip. Call once, after the display (and its I2C bus) has started.
void power_init(void);

// Read the current state. Returns false if the chip is not there.
bool power_read(muse_power_t *out);
