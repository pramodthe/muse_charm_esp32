// SPDX-License-Identifier: MIT
// Setup portal: SoftAP + web form + DNS catch-all so phones open the page
// automatically. Adapted from net_prov/portal.c in esp32-agui.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t portal_start(const char *ap_ssid);
void portal_stop(void);

// Block until the form is saved or timeout_ms passes. Returns true if saved.
bool portal_wait_saved(uint32_t timeout_ms);
