// SPDX-License-Identifier: MIT
// Behaviour: decides which clip plays and how the face looks.
//
//   idle            breathing loop, blinks and glances
//   tap             wave + "hi!"
//   tap the belly   giggle
//   hold            voice chat: listens while held, thinks, then answers
//                   (squeeze instead when no Deepgram key is set up)
//   BOOT button     squeeze while held, then cheer; hold 3 s for the setup page
//   swipe           brushed aside, glances that way
//   tilt            leans against the tilt to stay upright
//   shake           dizzy (interrupts anything)
//   voice           without voice chat: looks up, mouth follows your voice

#include "behavior.h"

#include <math.h>

#include "agent.h"
#include "audio.h"
#include "clips.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "setup_ui.h"
#include "wifi.h"

static const char *TAG = "behavior";

#ifdef CONFIG_MUSE_TILT_ENTER_DEG
#define TILT_ENTER_DEG ((float)CONFIG_MUSE_TILT_ENTER_DEG)
#else
#define TILT_ENTER_DEG 7.0f
#endif
#define TILT_EXIT_DEG fmaxf(TILT_ENTER_DEG - 3.0f, 1.0f)
#define LEAN_RANGE_DEG 18.0f  // the lean clip spans -18..+18 degrees
#define LEAN_FPS 14
#define HOLD_SQUEEZE_MAX_MS 8000
#define LISTEN_MAX_MS 15000
#define LISTEN_REPLY_MIN_MS 1500
#define TALK_HOLD_MAX_MS 30000
#define CHAT_MAX_MS 120000

static QueueHandle_t s_events;
static const muse_clip_t *s_idle, *s_wave, *s_giggle, *s_cheer, *s_lean, *s_dizzy, *s_listen, *s_squeeze;

static void tick(TickType_t *wake, int fps)
{
    vTaskDelayUntil(wake, pdMS_TO_TICKS(1000 / (fps > 0 ? fps : 10)));
}

// Play clip frames from..to (inclusive, either direction) at fps, or the
// clip's own rate when fps is 0. Returns false if a shake cut it short.
static bool play(const muse_clip_t *c, int from, int to, int fps, TickType_t *wake)
{
    const int step = to >= from ? 1 : -1;
    for (int i = from;; i += step) {
        if (g_muse.shake && c != s_dizzy) {
            return false;
        }
        clips_show(c->first + i);
        tick(wake, fps ? fps : c->fps);
        if (i == to) {
            return true;
        }
    }
}

static bool play_all(const muse_clip_t *c, TickType_t *wake)
{
    return play(c, 0, c->count - 1, 0, wake);
}

static void look(int mode, float x, float y)
{
    g_muse.gaze_x = x;
    g_muse.gaze_y = y;
    g_muse.gaze_mode = mode;
}

static void settle(void)
{
    g_muse.expr_override = -1;
    look(MUSE_GAZE_AUTO, 0, 0);
    xQueueReset(s_events);  // drop taps made during a reaction
}

// Wait while cond() holds, up to max_ms or until a shake.
static void hold_while(bool (*cond)(void), int max_ms, TickType_t *wake)
{
    const int64_t until = muse_now_ms() + max_ms;
    while (cond() && muse_now_ms() < until && !g_muse.shake) {
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    *wake = xTaskGetTickCount();
}

static bool finger_or_button_down(void)
{
    return g_muse.touch_down || g_muse.button_down;
}

static bool talking_to_muse(void)
{
    return g_muse.touch_down && !g_muse.agent_error;
}

static bool voice_active(void)
{
    return g_muse.voice_active;
}

// ---------------------------------------------------------------- reactions
static void react_dizzy(TickType_t *wake)
{
    g_muse.shake = false;
    audio_play(MUSE_SOUND_DIZZY);
    look(MUSE_GAZE_TARGET, 0, 0);
    play_all(s_dizzy, wake);
    g_muse.shake = false;
    g_muse.blink_request = true;
}

static void react_squeeze(TickType_t *wake)
{
    audio_play(MUSE_SOUND_SQUEEZE);
    if (!play_all(s_squeeze, wake)) {
        return;
    }
    hold_while(finger_or_button_down, HOLD_SQUEEZE_MAX_MS, wake);
    if (g_muse.shake) {
        return;
    }
    play(s_squeeze, s_squeeze->count - 1, 0, 20, wake);
    audio_play(MUSE_SOUND_CHEER);
    play_all(s_cheer, wake);
}

static void react_swipe(bool right, TickType_t *wake)
{
    const int mid = s_lean->count / 2;
    const int far = right ? s_lean->count - 2 : 1;
    audio_play(MUSE_SOUND_WHOOSH);
    g_muse.expr_override = MUSE_EXPR_SURPRISED;
    look(MUSE_GAZE_TARGET, right ? 1.0f : -1.0f, -0.2f);
    if (!play(s_lean, mid, far, 15, wake)) {
        return;
    }
    for (int i = 0; i < 8 && !g_muse.shake; i++) {
        tick(wake, 12);
    }
    g_muse.expr_override = MUSE_EXPR_NORMAL;
    play(s_lean, far, mid, 10, wake);
}

static void react_listen(TickType_t *wake)
{
    g_muse.expr_override = MUSE_EXPR_TALK;
    look(MUSE_GAZE_TARGET, 0.1f, -1.0f);
    const int64_t start = muse_now_ms();
    if (!play_all(s_listen, wake)) {
        return;
    }
    hold_while(voice_active, LISTEN_MAX_MS, wake);
    if (g_muse.shake) {
        return;
    }
    const bool long_talk = muse_now_ms() - start > LISTEN_REPLY_MIN_MS;
    g_muse.expr_override = MUSE_EXPR_NORMAL;
    look(MUSE_GAZE_AUTO, 0, 0);
    play(s_listen, s_listen->count - 1, 0, 0, wake);
    if (long_talk) {
        g_muse.blink_request = true;
        audio_play(MUSE_SOUND_MMHM);
    }
}

// Something went wrong (no WiFi, no key, Deepgram error): a puzzled head shake.
static void react_error(TickType_t *wake)
{
    g_muse.agent_error = false;
    audio_play(MUSE_SOUND_WHOOSH);
    g_muse.expr_override = MUSE_EXPR_SURPRISED;
    look(MUSE_GAZE_TARGET, 0, 0);
    const int mid = s_lean->count / 2;
    play(s_lean, mid, mid - 2, 15, wake);
    play(s_lean, mid - 2, mid + 2, 15, wake);
    play(s_lean, mid + 2, mid, 15, wake);
}

static void play_action(int action, TickType_t *wake)
{
    const muse_clip_t *clips[] = {s_wave, s_giggle, s_cheer, s_dizzy};
    if (action >= 0 && action < 4) {
        play_all(clips[action], wake);
    }
}

// Voice chat: listen while the screen is held, think, then answer with the
// mouth following the reply. Holding again while Muse talks interrupts it.
static void react_talk(TickType_t *wake)
{
    for (;;) {
        g_muse.agent_action = MUSE_ACTION_NONE;
        if (!agent_begin_turn()) {
            react_error(wake);
            return;
        }
        g_muse.expr_override = MUSE_EXPR_NORMAL;
        look(MUSE_GAZE_TARGET, 0.0f, -0.6f);
        play_all(s_listen, wake);  // tilt the head to listen
        hold_while(talking_to_muse, TALK_HOLD_MAX_MS, wake);
        agent_end_turn();

        bool tilted = true, interrupted = false;
        int idle = 0;
        const int64_t give_up = muse_now_ms() + CHAT_MAX_MS;
        while (muse_now_ms() < give_up && !g_muse.shake) {
            if (g_muse.agent_error) {
                react_error(wake);
                return;
            }
            muse_event_t ev;
            if (xQueueReceive(s_events, &ev, 0) == pdTRUE && ev == MUSE_EV_TALK) {
                agent_stop_speaking();  // held again: interrupt and listen
                interrupted = true;
                break;
            }
            const int state = g_muse.agent_state;
            if (state == MUSE_AGENT_SPEAKING) {
                if (tilted) {
                    play(s_listen, s_listen->count - 1, 0, 0, wake);
                    tilted = false;
                }
                g_muse.expr_override = MUSE_EXPR_TALK;
                look(MUSE_GAZE_TARGET, 0, 0);
                const int action = g_muse.agent_action;
                if (action != MUSE_ACTION_NONE) {
                    g_muse.agent_action = MUSE_ACTION_NONE;
                    play_action(action, wake);
                    continue;
                }
                clips_show(s_idle->first + idle);
                idle = (idle + 1) % s_idle->count;
                tick(wake, s_idle->fps);
            } else if (state == MUSE_AGENT_THINKING || state == MUSE_AGENT_CONNECTING ||
                       state == MUSE_AGENT_LISTENING) {
                // Thinking: keep the head tilted and glance up and aside.
                look(MUSE_GAZE_TARGET, -0.7f, -0.8f);
                tick(wake, 10);
            } else {
                break;  // the reply has finished, or none came
            }
        }
        if (!interrupted) {
            if (tilted) {
                play(s_listen, s_listen->count - 1, 0, 0, wake);
            }
            const int action = g_muse.agent_action;
            if (action != MUSE_ACTION_NONE) {
                g_muse.agent_action = MUSE_ACTION_NONE;
                play_action(action, wake);
            }
            return;
        }
    }
}

static void react_setup(TickType_t *wake)
{
    (void)wake;
    audio_play(MUSE_SOUND_WHOOSH);
    setup_ui_open();
}

static void react(muse_event_t ev, TickType_t *wake)
{
    switch (ev) {
    case MUSE_EV_TAP:
        audio_play(MUSE_SOUND_HELLO);
        play_all(s_wave, wake);
        break;
    case MUSE_EV_BELLY:
        audio_play(MUSE_SOUND_GIGGLE);
        play_all(s_giggle, wake);
        break;
    case MUSE_EV_LONG_PRESS:
        react_squeeze(wake);
        break;
    case MUSE_EV_SWIPE_LEFT:
    case MUSE_EV_SWIPE_RIGHT:
        react_swipe(ev == MUSE_EV_SWIPE_RIGHT, wake);
        break;
    case MUSE_EV_TALK:
        react_talk(wake);
        break;
    case MUSE_EV_BUTTON:
        react_squeeze(wake);
        break;
    case MUSE_EV_SETUP:
        react_setup(wake);
        break;
    }
}

// ---------------------------------------------------------------- main loop
static void behavior_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    audio_play(MUSE_SOUND_HELLO);  // say hello on power-up
    play_all(s_wave, &wake);
    settle();

    const int lean_mid = s_lean->count / 2;
    int lean = lean_mid;
    int idle = 0;
    bool leaning = false;
    for (;;) {
        if (g_muse.setup_request) {
            g_muse.setup_request = false;
            setup_ui_open();
        }
        if (setup_ui_active()) {
            vTaskDelay(pdMS_TO_TICKS(100));  // the setup screens are up
            wake = xTaskGetTickCount();
            continue;
        }
        if (g_muse.shake) {
            react_dizzy(&wake);
            settle();
            continue;
        }
        muse_event_t ev;
        if (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
            react(ev, &wake);
            settle();
            idle = 0;
            continue;
        }
        g_muse.agent_error = false;  // errors outside a conversation need no reaction
        if (g_muse.voice_active && !g_muse.touch_down) {
            react_listen(&wake);
            settle();
            continue;
        }

        // Tilt: show the lean pose for the angle, one step per tick so Muse
        // glides there, and glide back to upright afterwards.
        const float tilt = g_muse.tilt_deg;
        leaning = fabsf(tilt) > (leaning ? TILT_EXIT_DEG : TILT_ENTER_DEG);
        if (leaning || lean != lean_mid) {
            int target = lean_mid;
            if (leaning) {
                target = lean_mid + (int)lroundf(muse_clampf(tilt / LEAN_RANGE_DEG, -1.0f, 1.0f) * lean_mid);
                look(MUSE_GAZE_TARGET, g_muse.downhill, 0.3f);
            } else {
                look(MUSE_GAZE_AUTO, 0, 0);
            }
            if (lean != target || g_muse.frame != s_lean->first + lean) {
                lean += (target > lean) - (target < lean);
                clips_show(s_lean->first + lean);
            }
            tick(&wake, LEAN_FPS);
            continue;
        }

        clips_show(s_idle->first + idle);
        idle = (idle + 1) % s_idle->count;
        tick(&wake, s_idle->fps);
    }
}

bool behavior_init(void)
{
    s_idle = clips_find("idle");
    s_wave = clips_find("wave");
    s_giggle = clips_find("giggle");
    s_cheer = clips_find("cheer");
    s_lean = clips_find("lean");
    s_dizzy = clips_find("dizzy");
    s_listen = clips_find("listen");
    s_squeeze = clips_find("squeeze");
    if (!s_idle || !s_wave || !s_giggle || !s_cheer || !s_lean || !s_dizzy || !s_listen || !s_squeeze) {
        ESP_LOGE(TAG, "the frame table is missing clips; re-export assets/ from Blender");
        return false;
    }
    s_events = xQueueCreate(8, sizeof(muse_event_t));
    return s_events != NULL;
}

void behavior_start(void)
{
    xTaskCreatePinnedToCore(behavior_task, "muse_behavior", 6 * 1024, NULL, 3, NULL, 1);
}

void behavior_post(muse_event_t event)
{
    if (s_events) {
        xQueueSend(s_events, &event, 0);
    }
}
