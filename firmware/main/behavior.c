// SPDX-License-Identifier: MIT
// Behaviour: decides which clip plays and how the face looks.
//
//   idle            breathing loop, blinks and glances
//   tap             wave + "hi!"
//   tap the belly   giggle
//   hold the screen squeeze while held, then cheer; 5 s opens setup
//   hold BOOT       voice chat: listens while held, thinks, then answers with
//                   the body swaying and subtitles underneath (a squeeze
//                   instead when voice chat is not available; 3 s opens setup)
//   swipe           brushed aside, glances that way
//   tilt            leans against the tilt to stay upright
//   shake           dizzy (interrupts anything)
//   voice           without voice chat: looks up, mouth follows your voice
//
// Claude Code companion (companion.c), with a caption under Muse:
//   thinking        glances up and aside
//   working         reads the caption, left to right
//   needs you       waves for attention (up to three times)
//   hello / bye     waves;  done: cheers;  a tool failed: a puzzled look;
//   the turn failed: a head shake;  tidying memory: squeezes

#include "behavior.h"

#include <math.h>

#include "agent.h"
#include "audio.h"
#include "clips.h"
#include "companion.h"
#include "esp_log.h"
#include "face.h"
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
#define NUDGE_EVERY_MS 45000
#define NUDGE_TIMES 3

#ifdef CONFIG_MUSE_COMPANION_SOUNDS
#define COMPANION_SOUNDS 1
#else
#define COMPANION_SOUNDS 0
#endif

static QueueHandle_t s_events;
static int64_t s_caption_until_ms;  // an event's caption stays up until then
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
    return g_muse.button_down && !g_muse.agent_error;
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

static void head_shake(bool sound, TickType_t *wake)
{
    if (sound) {
        audio_play(MUSE_SOUND_WHOOSH);
    }
    g_muse.expr_override = MUSE_EXPR_SURPRISED;
    look(MUSE_GAZE_TARGET, 0, 0);
    const int mid = s_lean->count / 2;
    play(s_lean, mid, mid - 2, 15, wake);
    play(s_lean, mid - 2, mid + 2, 15, wake);
    play(s_lean, mid + 2, mid, 15, wake);
}

// Something went wrong (not linked, no key on the computer, Deepgram error):
// a puzzled head shake.
static void react_error(TickType_t *wake)
{
    g_muse.agent_error = false;
    head_shake(true, wake);
}

static void play_action(int action, TickType_t *wake)
{
    const muse_clip_t *clips[] = {s_wave, s_giggle, s_cheer, s_dizzy};
    if (action >= 0 && action < 4) {
        play_all(clips[action], wake);
    }
}

// Show the conversation's caption under Muse (agent.c decides what it says).
static void talk_caption(void)
{
    char caption[48];
    agent_caption(caption, sizeof(caption));
    face_set_caption(caption);
}

// Step the lean pose one frame toward target and show it.
static void lean_toward(int *lean, int target)
{
    *lean += (target > *lean) - (target < *lean);
    clips_show(s_lean->first + *lean);
}

// Voice chat: listen while BOOT is held, think, then answer. While Muse talks
// the whole body talks: the mouth follows the reply, the body sways a little
// on each syllable, gestures land a moment into the reply, and the words run
// underneath as subtitles. Holding BOOT again interrupts.
static void react_talk(TickType_t *wake)
{
    const int mid = s_lean->count / 2;
    for (;;) {
        g_muse.agent_action = MUSE_ACTION_NONE;
        if (!agent_begin_turn()) {
            react_error(wake);
            return;
        }
        audio_play(MUSE_SOUND_LISTEN);
        talk_caption();
        g_muse.expr_override = MUSE_EXPR_NORMAL;
        look(MUSE_GAZE_TARGET, 0.0f, -0.6f);
        play_all(s_listen, wake);  // tilt the head to listen
        hold_while(talking_to_muse, TALK_HOLD_MAX_MS, wake);
        agent_end_turn();
        audio_play(MUSE_SOUND_THINK);

        bool tilted = true, interrupted = false;
        int lean = mid, sway = 1;
        float last_level = 0;
        int64_t speaking_since = 0, quiet_since = 0;
        const int64_t give_up = muse_now_ms() + CHAT_MAX_MS;
        while (muse_now_ms() < give_up && !g_muse.shake) {
            if (g_muse.agent_error) {
                face_set_caption("");
                react_error(wake);
                return;
            }
            muse_event_t ev;
            if (xQueueReceive(s_events, &ev, 0) == pdTRUE && ev == MUSE_EV_TALK) {
                agent_stop_speaking();  // BOOT held again: interrupt and listen
                interrupted = true;
                break;
            }
            talk_caption();
            const int64_t now = muse_now_ms();
            const int state = g_muse.agent_state;
            if (state == MUSE_AGENT_SPEAKING) {
                if (tilted) {
                    play(s_listen, s_listen->count - 1, 0, 0, wake);
                    tilted = false;
                    speaking_since = now;
                }
                g_muse.expr_override = MUSE_EXPR_TALK;
                look(MUSE_GAZE_TARGET, 0, 0);
                const int action = g_muse.agent_action;
                if (action != MUSE_ACTION_NONE && now - speaking_since > 400) {
                    g_muse.agent_action = MUSE_ACTION_NONE;
                    play_action(action, wake);
                    lean = mid;
                    continue;
                }
                // Sway to the other side on each new syllable; settle in pauses
                // and blink at the end of a sentence.
                const float level = g_muse.mouth_level;
                if (level > 0.45f && last_level <= 0.3f) {
                    sway = -sway;
                }
                last_level = level;
                if (level < 0.12f) {
                    if (!quiet_since) {
                        quiet_since = now;
                    } else if (now - quiet_since > 350 && quiet_since > 0) {
                        g_muse.blink_request = true;
                        quiet_since = -1;  // once per pause
                    }
                } else {
                    quiet_since = 0;
                }
                lean_toward(&lean, quiet_since != 0 ? mid : mid + sway);
                tick(wake, 14);
            } else if (state == MUSE_AGENT_THINKING || state == MUSE_AGENT_CONNECTING ||
                       state == MUSE_AGENT_LISTENING) {
                // Thinking: keep the head tilted and glance up and aside.
                look(MUSE_GAZE_TARGET, (now / 1500) % 2 ? 0.5f : -0.7f, -0.8f);
                tick(wake, 10);
            } else {
                break;  // the reply has finished, or none came
            }
        }
        if (!interrupted) {
            // Settle: back to upright, a happy look, and let the last words linger.
            while (lean != mid) {
                lean_toward(&lean, mid);
                tick(wake, 14);
            }
            if (tilted) {
                play(s_listen, s_listen->count - 1, 0, 0, wake);
            }
            const int action = g_muse.agent_action;
            if (action != MUSE_ACTION_NONE) {
                g_muse.agent_action = MUSE_ACTION_NONE;
                play_action(action, wake);
            }
            if (!tilted || speaking_since) {
                g_muse.expr_override = MUSE_EXPR_HAPPY;
                for (int i = 0; i < 7; i++) {
                    talk_caption();
                    clips_show(s_idle->first + i);
                    tick(wake, s_idle->fps);
                }
            }
            s_caption_until_ms = muse_now_ms() + 1200;
            return;
        }
    }
}

// ---------------------------------------------------------------- Claude Code companion
static void chirp(muse_sound_t sound)
{
    if (COMPANION_SOUNDS) {
        audio_play(sound);
    }
}

static void react_claude(const companion_event_t *e, TickType_t *wake)
{
    int linger_ms = 3000;
    face_set_caption(e->caption);
    switch (e->kind) {
    case COMPANION_LINKED:
        chirp(MUSE_SOUND_MMHM);
        g_muse.blink_request = true;
        play_all(s_giggle, wake);
        linger_ms = 4000;
        break;
    case COMPANION_HELLO:
    case COMPANION_BYE:
        chirp(MUSE_SOUND_HELLO);
        play_all(s_wave, wake);
        linger_ms = e->kind == COMPANION_HELLO ? 4000 : 3000;
        break;
    case COMPANION_DONE:
        chirp(MUSE_SOUND_CHEER);
        look(MUSE_GAZE_TARGET, 0, 0);
        play_all(s_cheer, wake);
        linger_ms = 6000;
        break;
    case COMPANION_OOPS:
        head_shake(false, wake);  // quietly: commands fail all the time
        break;
    case COMPANION_ERROR:
        head_shake(COMPANION_SOUNDS, wake);
        linger_ms = 6000;
        break;
    case COMPANION_COMPACT:
        chirp(MUSE_SOUND_SQUEEZE);
        if (play_all(s_squeeze, wake)) {
            play(s_squeeze, s_squeeze->count - 1, 0, 0, wake);
        }
        break;
    }
    s_caption_until_ms = muse_now_ms() + linger_ms;
}

// Claude is waiting for your permission: wave to get your attention.
static void react_nudge(TickType_t *wake)
{
    char caption[COMPANION_CAPTION_LEN];
    companion_mood_caption(caption, sizeof(caption));
    face_set_caption(caption);
    chirp(MUSE_SOUND_WHOOSH);
    g_muse.expr_override = MUSE_EXPR_SURPRISED;
    look(MUSE_GAZE_TARGET, 0, 0);
    play_all(s_wave, wake);
}

// Where Muse looks while Claude works, and the caption under Muse.
static void follow_claude(void)
{
    const int64_t now = muse_now_ms();
    const int mood = g_muse.claude_mood;
    switch (mood) {
    case MUSE_MOOD_THINKING:
        look(MUSE_GAZE_TARGET, (now / 2600) % 2 ? 0.6f : -0.7f, -0.8f);  // up and aside
        break;
    case MUSE_MOOD_WORKING:
        look(MUSE_GAZE_TARGET, ((float)((now / 900) % 4) - 1.5f) * 0.35f, 0.9f);  // reading the caption
        break;
    case MUSE_MOOD_NEEDS_YOU:
        look(MUSE_GAZE_TARGET, 0, 0);
        break;
    default:
        look(MUSE_GAZE_AUTO, 0, 0);
        break;
    }
    if (now < s_caption_until_ms) {
        return;  // an event's caption is still up
    }
    char caption[COMPANION_CAPTION_LEN] = "";
    if (mood >= MUSE_MOOD_THINKING) {
        companion_mood_caption(caption, sizeof(caption));
    }
    face_set_caption(caption);
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
    int last_mood = MUSE_MOOD_OFF, nudges = 0;
    int64_t next_nudge_ms = 0;
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
        companion_event_t claude;
        if (companion_next_event(&claude)) {
            react_claude(&claude, &wake);
            settle();
            idle = 0;
            continue;
        }
        const int mood = g_muse.claude_mood;
        if (mood == MUSE_MOOD_NEEDS_YOU) {
            if (last_mood != MUSE_MOOD_NEEDS_YOU) {
                nudges = 0;
                next_nudge_ms = 0;
            }
            last_mood = mood;
            if (nudges < NUDGE_TIMES && muse_now_ms() >= next_nudge_ms) {
                react_nudge(&wake);
                nudges++;
                next_nudge_ms = muse_now_ms() + NUDGE_EVERY_MS;
                settle();
                idle = 0;
                continue;
            }
        }
        last_mood = mood;
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

        follow_claude();
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
