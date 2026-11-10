// SPDX-License-Identifier: MIT
// Muse charm: an interactive mascot for the Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// The body is pre-rendered in Blender as short reaction clips (baseline JPEG,
// packed at build time from assets/). The face is drawn live on top with LVGL
// at per-frame anchors exported from Blender. See README.md for the full tour.
//
//   clips.c     frame table, JPEG decoding, canvas
//   face.c      live eyes and mouth
//   behavior.c  which clip plays, and why
//   touch.c     drag, tap, belly tap, long press, swipe
//   motion.c    QMI8658 tilt and shake
//   audio.c     ES8311 microphone and speaker
//   sounds.c    the chirps

#include "audio.h"
#include "behavior.h"
#include "bsp/esp-bsp.h"
#include "clips.h"
#include "esp_log.h"
#include "face.h"
#include "motion.h"
#include "muse.h"
#include "touch.h"

static const char *TAG = "muse";

muse_state_t g_muse = {
    .expr_override = -1,
    .gaze_mode = MUSE_GAZE_AUTO,
};

void app_main(void)
{
    if (!clips_init() || !behavior_init()) {
        ESP_LOGE(TAG, "start-up failed");
        return;
    }

    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "display start failed");
        return;
    }
    bsp_display_brightness_set(CONFIG_MUSE_BRIGHTNESS);

    if (bsp_display_lock(0)) {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *canvas = clips_create_canvas(screen);
        touch_attach(canvas);
        face_create(screen);
        bsp_display_unlock();
    }

    audio_start();
    motion_start();
    behavior_start();
    ESP_LOGI(TAG, "Muse is awake: drag, tap, tap the belly, hold, swipe, tilt, shake or talk.");
}
