// SPDX-License-Identifier: MIT
// Panel watchdog. See panel.h.

#include "panel.h"

#include "bsp/esp-bsp.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "panel";

#define CHECK_EVERY_MS 2000
#define CO5300_READ (0x03u << 24)  // QSPI opcode for a register read
#define CMD_RDDPM 0x0A             // read display power mode
#define PM_SLEEP_OUT 0x10
#define PM_DISPLAY_ON 0x04

// esp_lvgl_port 2.x keeps the panel handles at the start of its display
// context (lvgl_port_display_ctx_t, the display's driver data). The BSP does
// not hand them out otherwise.
typedef struct {
    int disp_type;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
} port_ctx_head_t;

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static volatile bool s_wake;

// The panel's power mode, or -1 if it can't be read.
static int read_power_mode(void)
{
    uint8_t mode = 0;
    if (esp_lcd_panel_io_rx_param(s_io, CO5300_READ | (CMD_RDDPM << 8), &mode, 1) != ESP_OK) {
        return -1;
    }
    return mode;
}

static bool looks_on(int mode)
{
    return mode >= 0 && (mode & (PM_SLEEP_OUT | PM_DISPLAY_ON)) == (PM_SLEEP_OUT | PM_DISPLAY_ON);
}

// Call with the display lock held.
static void set_up_again(void)
{
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);
    bsp_display_brightness_set(CONFIG_MUSE_BRIGHTNESS);
    lv_obj_invalidate(lv_screen_active());  // the panel's memory is gone: redraw everything
}

static void guard_task(void *arg)
{
    (void)arg;
    bool can_read = false;
    if (bsp_display_lock(1000)) {
        const int mode = read_power_mode();
        bsp_display_unlock();
        can_read = looks_on(mode);
        if (can_read) {
            ESP_LOGI(TAG, "watching the panel (power mode 0x%02x)", mode);
        } else {
            ESP_LOGW(TAG, "can't read the panel's power mode (got %d); BOOT still wakes the screen", mode);
        }
    }
    int last = -2;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_wake ? 50 : CHECK_EVERY_MS));
        if (!bsp_display_lock(500)) {
            continue;
        }
        const int mode = can_read ? read_power_mode() : -1;
        const bool lost = can_read && !looks_on(mode);
        if (lost || s_wake) {
            if (lost) {
                ESP_LOGW(TAG, "the panel turned off (power mode 0x%02x); setting it up again", mode);
            } else {
                ESP_LOGI(TAG, "setting the panel up again");
            }
            s_wake = false;
            set_up_again();
            last = -2;
        } else if (mode != last && last != -2) {
            ESP_LOGI(TAG, "power mode 0x%02x", mode);
        }
        if (!lost) {
            last = mode;
        }
        bsp_display_unlock();
    }
}

void panel_guard_start(void)
{
    lv_display_t *disp = lv_display_get_default();
    const port_ctx_head_t *ctx = disp ? lv_display_get_driver_data(disp) : NULL;
    if (!ctx || !ctx->io || !ctx->panel) {
        ESP_LOGW(TAG, "no panel handles; not watching");
        return;
    }
    s_io = ctx->io;
    s_panel = ctx->panel;
    xTaskCreatePinnedToCore(guard_task, "muse_panel", 3072, NULL, 2, NULL, 0);
}

void panel_wake(void)
{
    s_wake = true;
}
