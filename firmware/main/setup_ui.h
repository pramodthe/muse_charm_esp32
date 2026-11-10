// SPDX-License-Identifier: MIT
// On-screen setup: pick a WiFi network and type its password on Muse's own
// touch screen.
#pragma once

#include <stdbool.h>

// Open the setup screens (no-op if already open). Call without the LVGL lock.
void setup_ui_open(void);

bool setup_ui_active(void);
