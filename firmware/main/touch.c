// SPDX-License-Identifier: MIT
// Touch gestures (drag, tap, belly tap, hold, swipe) and the BOOT button.
//
// While a finger is down its position is published in g_muse so the eyes can
// follow it. Gestures become events for the behaviour task.
//
//   screen held still   squeeze; keep holding 5 s for setup
//   BOOT held           talk to Muse (push to talk) when voice chat is on;
//                       otherwise a squeeze, and 3 s opens setup

#include "touch.h"

#include <stdlib.h>

#include "behavior.h"
#include "clips.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "muse.h"
#include "setup_ui.h"

#define BOOT_BUTTON GPIO_NUM_0
#define BUTTON_POLL_MS 30
#define BUTTON_DEBOUNCE_POLLS 4    // ~100 ms low: supply glitches on GPIO0 are shorter
#define SETUP_HOLD_MS 3000         // BOOT, without voice chat
#define SCREEN_SETUP_HOLD_MS 5000
#define LONG_PRESS_MS 550
#define TAP_MAX_MS 450
#define TAP_MAX_MOVE_PX 24
#define SWIPE_MIN_DX_PX 60
#define BELLY_RX_PX 75.0f
#define BELLY_RY_PX 65.0f

static int64_t s_press_ms;
static int s_press_x, s_press_y, s_max_move;
static bool s_long_fired, s_setup_fired;

static bool on_belly(int x, int y)
{
    const muse_frames_header_t *hdr = clips_header();
    const float bx = (x - (float)hdr->belly_x) / BELLY_RX_PX;
    const float by = (y - (float)hdr->belly_y) / BELLY_RY_PX;
    return bx * bx + by * by <= 1.0f;
}

static void on_touch(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int64_t now = muse_now_ms();

    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_press_ms = now;
        s_press_x = p.x;
        s_press_y = p.y;
        s_max_move = 0;
        s_long_fired = false;
        s_setup_fired = false;
        g_muse.touch_x = p.x;
        g_muse.touch_y = p.y;
        g_muse.touch_down = true;
        break;

    case LV_EVENT_PRESSING: {
        g_muse.touch_x = p.x;
        g_muse.touch_y = p.y;
        const int dx = abs(p.x - s_press_x), dy = abs(p.y - s_press_y);
        const int move = dx > dy ? dx : dy;
        if (move > s_max_move) {
            s_max_move = move;
        }
        if (!s_long_fired && s_max_move < TAP_MAX_MOVE_PX && now - s_press_ms >= LONG_PRESS_MS) {
            s_long_fired = true;
            behavior_post(MUSE_EV_LONG_PRESS);
        }
        if (s_long_fired && !s_setup_fired && s_max_move < TAP_MAX_MOVE_PX &&
                now - s_press_ms >= SCREEN_SETUP_HOLD_MS) {
            s_setup_fired = true;
            behavior_post(MUSE_EV_SETUP);
        }
        break;
    }

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
        g_muse.touch_down = false;
        if (s_long_fired) {
            break;  // the squeeze ends when behaviour sees the finger lift
        }
        const int dx = p.x - s_press_x, dy = p.y - s_press_y;
        if (abs(dx) >= SWIPE_MIN_DX_PX && abs(dx) > abs(dy) * 3 / 2) {
            behavior_post(dx > 0 ? MUSE_EV_SWIPE_RIGHT : MUSE_EV_SWIPE_LEFT);
        } else if (s_max_move < TAP_MAX_MOVE_PX && now - s_press_ms <= TAP_MAX_MS) {
            behavior_post(on_belly(p.x, p.y) ? MUSE_EV_BELLY : MUSE_EV_TAP);
        }
        break;
    }

    default:
        break;
    }
}

// BOOT button: hold to talk when voice chat is on; otherwise a squeeze, and
// holding 3 s opens setup.
static void poll_button(lv_timer_t *t)
{
    (void)t;
    static bool was_down, setup_fired, talking;
    static int low_polls;
    static int64_t pressed_ms;
    // Several low readings in a row, so a supply glitch isn't a press.
    const bool low = gpio_get_level(BOOT_BUTTON) == 0;
    low_polls = low ? low_polls + 1 : 0;
    const bool down = !setup_ui_active() && (was_down ? low : low_polls >= BUTTON_DEBOUNCE_POLLS);
    const int64_t now = muse_now_ms();
    if (down && !was_down) {
        talking = g_muse.agent_state != MUSE_AGENT_OFF;
        ESP_LOGI("touch", "BOOT pressed%s", talking ? ": talk" : "");
        pressed_ms = now;
        setup_fired = false;
        g_muse.button_down = true;
        behavior_post(talking ? MUSE_EV_TALK : MUSE_EV_BUTTON);
    } else if (down && !talking && !setup_fired && now - pressed_ms >= SETUP_HOLD_MS) {
        setup_fired = true;
        behavior_post(MUSE_EV_SETUP);
    } else if (!down && was_down) {
        g_muse.button_down = false;
    }
    was_down = down;
}

void touch_attach(lv_obj_t *obj)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    lv_timer_create(poll_button, BUTTON_POLL_MS, NULL);

    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(obj, on_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, on_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, on_touch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, on_touch, LV_EVENT_PRESS_LOST, NULL);
}
