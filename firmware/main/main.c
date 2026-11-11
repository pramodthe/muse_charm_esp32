// SPDX-License-Identifier: MIT
// Muse charm: an interactive mascot for the Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// The body is pre-rendered in Blender as short reaction clips (baseline JPEG,
// packed at build time from assets/). The face is drawn live on top with LVGL
// at per-frame anchors exported from Blender. See README.md for the full tour.
//
//   clips.c     frame table, JPEG decoding, canvas
//   camera.c    which part of the frame is on screen (zoom.c enlarges it)
//   panel.c     sets the AMOLED panel up again if it turns off
//   face.c      live eyes and mouth
//   behavior.c  which clip plays, and why
//   touch.c     drag, tap, belly tap, long press, swipe
//   motion.c    QMI8658 tilt and shake
//   audio.c     ES8311 microphone and speaker
//   sounds.c    the chirps
//   agent.c     voice chat, relayed by the companion server
//   companion.c shows what Claude Code is doing on your computer
//   wifi.c      saved networks and reconnects; portal.c the setup page
//   settings.c  NVS start-up

#include "agent.h"
#include "audio.h"
#include "behavior.h"
#include "bsp/esp-bsp.h"
#include "clips.h"
#include "companion.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "face.h"
#include "motion.h"
#include "muse.h"
#include "panel.h"
#include "settings.h"
#include "touch.h"
#include "wifi.h"

static const char *TAG = "muse";

extern volatile uint32_t g_face_ticks;

// Report if the display task stops (it would otherwise fail silently), plus
// memory, so problems show up in the log.
static void watchdog_task(void *arg)
{
    (void)arg;
    uint32_t last = g_face_ticks;
    int quiet = 0;
    for (int n = 0;; n++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const uint32_t now = g_face_ticks;
        quiet = now == last ? quiet + 1 : 0;
        last = now;
        if (quiet == 3) {
            ESP_LOGE(TAG, "display task stalled (internal RAM free %u KB, largest %u KB)",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        }
#ifdef CONFIG_MUSE_DEBUG_LOG
        if (n % 10 == 0) {
            ESP_LOGI(TAG, "internal RAM free %u KB (largest %u KB)",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
        }
#endif
    }
}

muse_state_t g_muse = {
    .expr_override = -1,
    .gaze_mode = MUSE_GAZE_AUTO,
    .agent_action = MUSE_ACTION_NONE,
};

void app_main(void)
{
    ESP_ERROR_CHECK(settings_init());
    if (!clips_init() || !behavior_init()) {
        ESP_LOGE(TAG, "start-up failed");
        return;
    }

    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "display start failed");
        return;
    }
    if (bsp_display_lock(0)) {
        // The brightness command shares the QSPI bus with LVGL's flushes, so send
        // it with the display lock held (a collision can leave the panel dark).
        bsp_display_brightness_set(CONFIG_MUSE_BRIGHTNESS);

        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *canvas = clips_create_canvas(screen);
        touch_attach(canvas);
        face_create(screen);
        bsp_display_unlock();
    }
    panel_guard_start();

#if defined(CONFIG_MUSE_AGENT_ENABLE) || defined(CONFIG_MUSE_COMPANION_ENABLE)
    wifi_start();
#endif
#ifdef CONFIG_MUSE_AGENT_ENABLE
    agent_init();
#endif
#ifdef CONFIG_MUSE_COMPANION_ENABLE
    companion_start();
#endif
    audio_start();
    motion_start();
    behavior_start();
    xTaskCreatePinnedToCore(watchdog_task, "muse_watch", 3072, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "Muse is awake: drag, tap, tap the belly, hold to talk, swipe, tilt or shake.");
    ESP_LOGI(TAG, "internal RAM free %u KB (largest block %u KB), PSRAM free %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
