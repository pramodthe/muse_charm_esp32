// SPDX-License-Identifier: MIT
// NVS start-up. Adapted from app_cfg in esp32-agui.

#include "settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define OLD_NAMESPACE "muse"  // Deepgram key, voice and personality (0.3.0)

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    nvs_handle_t h;
    if (nvs_open(OLD_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        size_t used = 0;
        if (nvs_get_used_entry_count(h, &used) == ESP_OK && used > 0) {
            nvs_erase_all(h);
            nvs_commit(h);
            ESP_LOGI("settings", "removed the voice settings saved on the board; they now live on the computer");
        }
        nvs_close(h);
    }
    return ESP_OK;
}
