// SPDX-License-Identifier: MIT
// Small NVS-backed store for user settings (Deepgram key, voice, personality).
// Adapted from app_cfg in esp32-agui.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SETTING_DEEPGRAM_KEY "dg_key"
#define SETTING_VOICE "dg_voice"
#define SETTING_PERSONALITY "dg_prompt"

#define SETTING_MAX_LEN 1024  // longest stored value, including the terminator

// Initialise NVS (erasing it if its layout changed). Call once at start-up.
esp_err_t settings_init(void);

// Store a string; an empty or NULL value clears the key.
esp_err_t settings_set(const char *key, const char *value);

// Read a string into buf. Returns false if it is missing or empty.
bool settings_get(const char *key, char *buf, size_t len);

bool settings_has(const char *key);
