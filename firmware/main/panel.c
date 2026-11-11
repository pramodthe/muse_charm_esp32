// SPDX-License-Identifier: MIT
// Panel keeper. See panel.h.

#include "panel.h"

#include "bsp/esp-bsp.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "panel";

#define REFRESH_EVERY_MS 3000
#define CO5300_WRITE (0x02u << 24)  // QSPI opcode for a register write

// esp_lvgl_port 2.x keeps the panel IO handle near the start of its display
// context (lvgl_port_display_ctx_t, the display's driver data). The BSP does
// not hand it out otherwise.
typedef struct {
    int disp_type;
    esp_lcd_panel_io_handle_t io;
} port_ctx_head_t;

static esp_lcd_panel_io_handle_t s_io;

static void reg(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_lcd_panel_io_tx_param(s_io, CO5300_WRITE | ((uint32_t)cmd << 8), data, len);
}

// The settings the BSP sends at start-up (lcd_init_cmds in the board package),
// except the drawing window, which LVGL sets for every frame. Call with the
// display lock held.
static void write_settings(void)
{
    const uint8_t brightness = (uint8_t)(CONFIG_MUSE_BRIGHTNESS * 255 / 100);
    reg(0xFE, (const uint8_t[]){0x00}, 1);  // user command page
    reg(0xC4, (const uint8_t[]){0x80}, 1);  // QSPI interface
    reg(0x3A, (const uint8_t[]){0x55}, 1);  // 16-bit colour
    reg(0x35, (const uint8_t[]){0x00}, 1);  // tearing effect line on
    reg(0x53, (const uint8_t[]){0x20}, 1);  // brightness control on
    reg(0x63, (const uint8_t[]){0xFF}, 1);  // HBM brightness
    reg(0x51, &brightness, 1);              // brightness
    reg(0x11, NULL, 0);                     // sleep out (no effect when awake)
    vTaskDelay(pdMS_TO_TICKS(10));
    reg(0x29, NULL, 0);                     // display on (no effect when on)
}

static void keeper_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(REFRESH_EVERY_MS));
        if (bsp_display_lock(500)) {
            write_settings();
            bsp_display_unlock();
        }
    }
}

void panel_guard_start(void)
{
    lv_display_t *disp = lv_display_get_default();
    const port_ctx_head_t *ctx = disp ? lv_display_get_driver_data(disp) : NULL;
    if (!ctx || !ctx->io) {
        ESP_LOGW(TAG, "no panel handle; not refreshing the panel settings");
        return;
    }
    s_io = ctx->io;
    xTaskCreatePinnedToCore(keeper_task, "muse_panel", 3072, NULL, 2, NULL, 0);
}
