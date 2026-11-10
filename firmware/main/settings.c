// SPDX-License-Identifier: MIT
// Small NVS-backed store for user settings. Adapted from app_cfg in esp32-agui.

#include "settings.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define NAMESPACE "muse"

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t settings_set(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (value && value[0]) {
        err = nvs_set_str(h, key, value);
    } else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool settings_get(const char *key, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return false;
    }
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = len;
    const esp_err_t err = nvs_get_str(h, key, buf, &n);
    nvs_close(h);
    return err == ESP_OK && buf[0] != '\0';
}

bool settings_has(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = 0;
    const esp_err_t err = nvs_get_str(h, key, NULL, &n);
    nvs_close(h);
    return err == ESP_OK && n > 1;
}
