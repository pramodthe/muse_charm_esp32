// SPDX-License-Identifier: MIT
// WiFi: saved networks, auto-reconnect and the "Muse-setup" portal.
// Adapted from net_prov in esp32-agui.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#define WIFI_SETUP_SSID "Muse-setup"

// Start WiFi in the background: connect to a saved network, or ask for setup
// on the screen when none is saved or none is in range.
void wifi_start(void);

bool wifi_connected(void);
bool wifi_has_credentials(void);

// Open the setup portal (SoftAP + web page) for 10 minutes or until saved.
void wifi_open_setup(void);
bool wifi_setup_active(void);

// Saved-network list (used by the portal).
esp_err_t wifi_creds_add(const char *ssid, const char *pass);

// For the on-screen setup: pause background connecting, scan, and join a
// network (saved on success).
void wifi_hold(bool hold);
int wifi_scan(wifi_ap_record_t *out, int max);
bool wifi_try_connect(const char *ssid, const char *pass, uint32_t timeout_ms);
bool wifi_current_ssid(char *buf, size_t len);
