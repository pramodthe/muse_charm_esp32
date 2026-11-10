// SPDX-License-Identifier: MIT
// NVS start-up. Voice chat settings and the Deepgram key live on the computer,
// with the companion server.
#pragma once

#include "esp_err.h"

// Initialise NVS (saved WiFi networks), and remove the Deepgram key, voice and
// personality that firmware 0.3.0 stored on the board.
esp_err_t settings_init(void);
