// SPDX-License-Identifier: MIT
// Behaviour: decides which clip plays, how the face looks and where the
// camera is.
//
//   idle            breathing loop, blinks and glances; now and then a fidget
//                   (a peek right up to the glass, a look around, a wiggle)
//   quiet minutes   dozes off: eyes closed, "z z Z", screen dimmed; wakes when
//                   touched, picked up, spoken to or when Claude Code needs it
//   picked up       looks at you, pleased
//   tap             wave + "hi!"; on the belly a giggle; on the face a boop
//   stroke the head petting: happy eyes, a purr, the head leans into your hand
//   hold the screen squeeze while held, then cheer; 5 s opens setup
//   hold BOOT       voice chat: comes close and listens while held, thinks,
//                   under a thought cloud, then answers in close-up with the
//                   body swaying and subtitles underneath. The AI moves the
//                   body with its tools: wave, dance, come closer, sleep...
//                   (a squeeze instead when voice chat is not available;
//                   3 s opens setup)
//   swipe           brushed aside, glances that way
//   tilt            leans against the tilt to stay upright
//   shake           dizzy (interrupts anything)
//   voice           without voice chat: looks up, mouth follows your voice
//
// Claude Code companion (companion.c), with a caption under Muse:
//   thinking        a thought cloud, glancing up at it
//   working         reads the caption, left to right
//   needs you       comes up to the glass and waves (up to three times)
//   hello / bye     waves;  done: cheers;  a tool failed: a puzzled look;
//   the turn failed: a head shake;  tidying memory: squeezes

#include "behavior.h"

#include <math.h>

#include "agent.h"
#include "audio.h"
#include "camera.h"
#include "clips.h"
#include "companion.h"
#include "esp_log.h"
#include "esp_random.h"
#include "face.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "panel.h"
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
#define LEAN_IN 0.05f            // how much closer Muse leans while you speak
#define PET_MAX_MS 30000
#define PET_PURR_EVERY_MS 900
#define PICKUP_REST_MS 20000     // still this long before a pick-up gets a greeting
#define FIDGET_MIN_MS 22000      // a fidget every 22-52 s while nothing happens
#define FIDGET_SPAN_MS 30000
#define FIDGET_AFTER_ACTION_MS 12000
#define SLEEP_FPS 5              // slow breathing

#ifdef CONFIG_MUSE_FIDGETS
#define FIDGETS 1
#else
#define FIDGETS 0
#endif
#ifdef CONFIG_MUSE_SLEEP_AFTER_S
#define SLEEP_AFTER_MS (CONFIG_MUSE_SLEEP_AFTER_S * 1000)
#define SLEEP_BRIGHTNESS CONFIG_MUSE_SLEEP_BRIGHTNESS
#else
#define SLEEP_AFTER_MS 180000
#define SLEEP_BRIGHTNESS 20
#endif

#ifdef CONFIG_MUSE_COMPANION_SOUNDS
#define COMPANION_SOUNDS 1
#else
#define COMPANION_SOUNDS 0
#endif

static QueueHandle_t s_events;
static int64_t s_caption_until_ms;  // an event's caption stays up until then
static int64_t s_active_ms;         // when something last happened (for dozing off)
static int64_t s_next_fidget_ms;
static int s_idle_pos;              // where the breathing loop is
static muse_shot_t s_talk_shot;     // the shot for a reply; the AI can change it
static int s_emote = -1;            // a face the AI asked for, until s_emote_until_ms
static int64_t s_emote_until_ms;
static bool s_sleep_asked;          // the AI said good night
static float s_home_x;              // where the face rests, in frame pixels
static const muse_clip_t *s_idle, *s_wave, *s_giggle, *s_cheer, *s_lean, *s_dizzy, *s_listen, *s_squeeze;

static void tick(TickType_t *wake, int fps)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / (fps > 0 ? fps : 10));
    const TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - *wake) >= period) {
        // Running late (a heavy frame): don't race to catch up afterwards, but
        // do let other tasks in.
        *wake = now;
        vTaskDelay(1);
        return;
    }
    vTaskDelayUntil(wake, period);
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

// Back to the resting look and the wide shot.
static void rest(void)
{
    g_muse.expr_override = -1;
    g_muse.thought = false;
    look(MUSE_GAZE_AUTO, 0, 0);
    camera_lean(0);
    camera_shot(MUSE_SHOT_WIDE, 450);
}

static void settle(void)
{
    rest();
    xQueueReset(s_events);  // drop taps made during a reaction
    s_idle_pos = 0;
}

// Something happened: Muse stays awake, and holds off fidgeting for a bit.
static void active(void)
{
    s_active_ms = muse_now_ms();
    if (s_next_fidget_ms < s_active_ms + FIDGET_AFTER_ACTION_MS) {
        s_next_fidget_ms = s_active_ms + FIDGET_AFTER_ACTION_MS + esp_random() % FIDGET_SPAN_MS;
    }
}

// Muse has something to say without being asked (a timer is up, the battery
// is low, good morning): the server has started a reply that no turn asked for.
static bool speaking_up(void)
{
    const int state = g_muse.agent_state;
    return state == MUSE_AGENT_SPEAKING || state == MUSE_AGENT_THINKING;
}

// Someone wants Muse's attention: idle business should step aside.
static bool busy(void)
{
    return g_muse.touch_down || g_muse.button_down || g_muse.shake || g_muse.setup_request ||
           uxQueueMessagesWaiting(s_events) > 0 || companion_event_waiting() || speaking_up();
}

// One frame of the breathing loop.
static void breathe(TickType_t *wake, int fps)
{
    clips_show(s_idle->first + s_idle_pos);
    s_idle_pos = (s_idle_pos + 1) % s_idle->count;
    tick(wake, fps ? fps : s_idle->fps);
}

// Breathe for a while. Returns false if stop() (when given) or a shake cut it short.
static bool breathe_for(int ms, bool (*stop)(void), TickType_t *wake)
{
    const int64_t until = muse_now_ms() + ms;
    while (muse_now_ms() < until) {
        if (g_muse.shake || (stop && stop())) {
            return false;
        }
        breathe(wake, 0);
    }
    return true;
}

// Wait while cond() holds, up to max_ms or until a shake, keeping the pose
// (and following the camera if it is still moving).
static void hold_while(bool (*cond)(void), int max_ms, TickType_t *wake)
{
    const int64_t until = muse_now_ms() + max_ms;
    while (cond() && muse_now_ms() < until && !g_muse.shake) {
        clips_refresh();
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

// A little dance: sway to a bar of music, then a flourish, three times.
static void dance(TickType_t *wake)
{
    static const int8_t steps[] = {3, -3, 3, -3, 0};
    const int mid = s_lean->count / 2;
    int lean = mid;
    look(MUSE_GAZE_TARGET, 0, 0);
    for (int bar = 0; bar < 3 && !g_muse.shake; bar++) {
        g_muse.expr_override = MUSE_EXPR_HAPPY;
        audio_play(MUSE_SOUND_DANCE);
        for (unsigned s = 0; s < sizeof(steps) && !g_muse.shake; s++) {
            while (lean != mid + steps[s]) {
                lean_toward(&lean, mid + steps[s]);
                tick(wake, 18);
            }
        }
        if (bar == 0) {
            play_all(s_giggle, wake);
        } else if (bar == 1) {
            if (play(s_squeeze, 0, s_squeeze->count - 1, 20, wake)) {
                play(s_squeeze, s_squeeze->count - 1, 0, 20, wake);
            }
        } else {
            play_all(s_cheer, wake);
        }
    }
}

// Do one of the moves the AI asked for (its muse_do tool). Body moves step the
// camera back so they are in view; faces and camera moves just take effect.
// Returns true if the body moved.
static bool perform(int move, TickType_t *wake)
{
    const int mid = s_lean->count / 2;
    const bool whole_body = move == MUSE_MOVE_WAVE || move == MUSE_MOVE_GIGGLE || move == MUSE_MOVE_CHEER ||
                            move == MUSE_MOVE_DANCE || move == MUSE_MOVE_SPIN || move == MUSE_MOVE_BOUNCE;
    if (whole_body && s_talk_shot != MUSE_SHOT_WIDE) {
        camera_shot(MUSE_SHOT_MEDIUM, 300);
    }
    int lean = mid;
    switch (move) {
    case MUSE_MOVE_WAVE:
        play_all(s_wave, wake);
        break;
    case MUSE_MOVE_GIGGLE:
        play_all(s_giggle, wake);
        break;
    case MUSE_MOVE_CHEER:
        play_all(s_cheer, wake);
        break;
    case MUSE_MOVE_DANCE:
        dance(wake);
        break;
    case MUSE_MOVE_SPIN:
        play_all(s_dizzy, wake);
        g_muse.shake = false;
        break;
    case MUSE_MOVE_SHAKE_HEAD:
        head_shake(false, wake);
        break;
    case MUSE_MOVE_TILT_HEAD:
        g_muse.expr_override = MUSE_EXPR_NORMAL;
        if (play_all(s_listen, wake)) {
            for (int i = 0; i < 6; i++) {
                clips_refresh();
                tick(wake, 10);
            }
            play(s_listen, s_listen->count - 1, 0, 0, wake);
        }
        break;
    case MUSE_MOVE_BOUNCE:
        g_muse.expr_override = MUSE_EXPR_HAPPY;
        for (int i = 0; i < 2 && play(s_squeeze, 0, s_squeeze->count - 1, 20, wake); i++) {
            play(s_squeeze, s_squeeze->count - 1, 0, 20, wake);
        }
        break;
    case MUSE_MOVE_SWAY:
        for (int i = 0; i < 4 && !g_muse.shake; i++) {
            const int to = mid + (i % 2 ? -2 : 2);
            while (lean != to) {
                lean_toward(&lean, to);
                tick(wake, 12);
            }
        }
        while (lean != mid) {
            lean_toward(&lean, mid);
            tick(wake, 12);
        }
        break;
    case MUSE_MOVE_SMILE:
    case MUSE_MOVE_SURPRISED:
        s_emote = move == MUSE_MOVE_SMILE ? MUSE_EXPR_HAPPY : MUSE_EXPR_SURPRISED;
        s_emote_until_ms = muse_now_ms() + 1600;
        g_muse.expr_override = s_emote;
        return false;
    case MUSE_MOVE_COME_CLOSER:
        s_talk_shot = MUSE_SHOT_PEEK;
        camera_shot(s_talk_shot, 450);
        return false;
    case MUSE_MOVE_STEP_BACK:
        s_talk_shot = MUSE_SHOT_WIDE;
        camera_shot(s_talk_shot, 450);
        return false;
    case MUSE_MOVE_SLEEP:
        s_sleep_asked = true;
        return false;
    default:
        return false;
    }
    if (whole_body) {
        camera_shot(s_talk_shot, 400);
    }
    return true;
}

// Voice chat: listen while BOOT is held, think, then answer. The camera
// directs it like a scene: Muse comes close and tilts its head to listen,
// leaning in a little as you speak; it answers in close-up, the mouth
// following the words and the body swaying on each syllable, with subtitles
// underneath; it steps back for a gesture so the arms are in view, and pulls
// back to the wide shot at the end. While it thinks, a thought cloud hangs
// over its head. The AI can move the body with its tools (perform() above).
// Holding BOOT again interrupts.
//
// `asked` is false when Muse speaks first (a timer, a low battery, good
// morning): there is nothing to listen to, so it comes close and says it.
// Holding BOOT then answers it like any other turn.
static void react_talk(TickType_t *wake, bool asked)
{
    const int mid = s_lean->count / 2;
    for (;;) {
        s_talk_shot = MUSE_SHOT_CLOSE;
        s_emote_until_ms = 0;
        s_sleep_asked = false;
        g_muse.thought = false;
        float lean_in = 0;
        if (asked) {
            if (!agent_begin_turn()) {
                react_error(wake);
                return;
            }
            audio_play(MUSE_SOUND_LISTEN);
            talk_caption();
            camera_shot(MUSE_SHOT_CLOSE, 450);
            g_muse.expr_override = MUSE_EXPR_NORMAL;
            look(MUSE_GAZE_TARGET, 0.0f, -0.15f);  // at you
            play_all(s_listen, wake);               // tilt the head to listen
            const int64_t hold_until = muse_now_ms() + TALK_HOLD_MAX_MS;
            while (talking_to_muse() && muse_now_ms() < hold_until && !g_muse.shake) {
                lean_in += (g_muse.mic_level * LEAN_IN - lean_in) * 0.25f;
                camera_lean(lean_in);
                talk_caption();
                clips_refresh();
                vTaskDelay(pdMS_TO_TICKS(40));
            }
            *wake = xTaskGetTickCount();
            agent_end_turn();
            audio_play(MUSE_SOUND_THINK);
        } else {
            ESP_LOGI(TAG, "speaking first");
            talk_caption();
            camera_shot(MUSE_SHOT_CLOSE, 450);
            g_muse.expr_override = MUSE_EXPR_NORMAL;
            look(MUSE_GAZE_TARGET, 0, 0);
        }

        bool tilted = asked, interrupted = false;
        asked = true;  // from here on, BOOT starts an ordinary turn
        int lean = mid, sway = 1;
        float last_level = 0;
        int64_t speaking_since = 0, quiet_since = 0, move_wait_since = 0, moved_at = 0;
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
            lean_in *= 0.6f;
            camera_lean(lean_in);
            const int64_t now = muse_now_ms();
            const int state = g_muse.agent_state;
            if (state == MUSE_AGENT_SPEAKING) {
                if (tilted) {
                    play(s_listen, s_listen->count - 1, 0, 0, wake);
                    tilted = false;
                }
                if (!speaking_since) {
                    speaking_since = now;
                }
                g_muse.thought = false;
                look(MUSE_GAZE_TARGET, 0, 0);
                // What the AI asked the body to do, a moment into the reply.
                if (now - speaking_since > 400 && agent_move_waiting()) {
                    if (perform(agent_next_move(), wake)) {
                        lean = mid;
                    }
                    continue;
                }
                g_muse.expr_override = now < s_emote_until_ms ? s_emote : MUSE_EXPR_TALK;
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
                // Thinking: head tilted, a thought cloud overhead, eyes on it.
                g_muse.thought = !moved_at;
                look(MUSE_GAZE_TARGET, 0.6f, -0.9f);
                if (!agent_move_waiting()) {
                    move_wait_since = 0;
                } else if (!move_wait_since) {
                    move_wait_since = now;
                } else if (now - move_wait_since > 1500) {
                    // The AI answered with moves and no words (yet): do them.
                    g_muse.thought = false;
                    if (tilted) {
                        play(s_listen, s_listen->count - 1, 0, 0, wake);
                        tilted = false;
                    }
                    perform(agent_next_move(), wake);
                    moved_at = muse_now_ms();
                    continue;
                }
                if (moved_at && !agent_move_waiting() && now - moved_at > 4000) {
                    agent_done_waiting();  // moves were the whole answer
                    continue;
                }
                clips_refresh();
                tick(wake, 10);
            } else {
                break;  // the reply has finished, or none came
            }
        }
        camera_lean(0);
        g_muse.thought = false;
        if (!interrupted) {
            // Settle: back to upright, a happy look as the camera pulls back,
            // and let the last words linger.
            while (lean != mid) {
                lean_toward(&lean, mid);
                tick(wake, 14);
            }
            if (tilted) {
                play(s_listen, s_listen->count - 1, 0, 0, wake);
            }
            while (agent_move_waiting() && !g_muse.shake) {
                perform(agent_next_move(), wake);  // whatever the AI asked for that is still to do
            }
            if (s_talk_shot == MUSE_SHOT_PEEK) {
                breathe_for(1200, busy, wake);  // stay close a moment longer
            }
            camera_shot(MUSE_SHOT_WIDE, 700);
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

// ---------------------------------------------------------------- touch
// A tap on the face: a startled "oh!", then a giggle, up close.
static void react_boop(TickType_t *wake)
{
    camera_shot(MUSE_SHOT_CLOSE, 250);
    audio_play(MUSE_SOUND_WHOOSH);
    g_muse.expr_override = MUSE_EXPR_SURPRISED;
    look(MUSE_GAZE_TARGET, 0, 0);
    if (!breathe_for(450, NULL, wake)) {
        return;
    }
    g_muse.blink_request = true;
    g_muse.expr_override = MUSE_EXPR_HAPPY;
    audio_play(MUSE_SOUND_GIGGLE);
    play_all(s_giggle, wake);
}

// Stroking the head: happy eyes, a purr, and the head leans into your hand
// for as long as the finger keeps moving there.
static void react_pet(TickType_t *wake)
{
    const int mid = s_lean->count / 2;
    int lean = mid;
    int64_t next_purr = 0;
    camera_shot(MUSE_SHOT_MEDIUM, 500);
    g_muse.expr_override = MUSE_EXPR_HAPPY;
    const int64_t until = muse_now_ms() + PET_MAX_MS;
    while (g_muse.petting && muse_now_ms() < until && !g_muse.shake) {
        const int64_t now = muse_now_ms();
        if (now >= next_purr) {
            audio_play(MUSE_SOUND_PURR);
            next_purr = now + PET_PURR_EVERY_MS;
        }
        const float side = muse_clampf((g_muse.touch_fx - s_home_x) / 40.0f, -2.0f, 2.0f);
        lean_toward(&lean, mid + (int)lroundf(side));
        tick(wake, 12);
    }
    if (g_muse.shake) {
        return;
    }
    // A contented moment, then back upright.
    g_muse.blink_request = true;
    audio_play(MUSE_SOUND_MMHM);
    for (int i = 0; i < 6 || lean != mid; i++) {
        lean_toward(&lean, mid);
        tick(wake, 12);
    }
}

// Picked up after resting: looks at you, pleased.
static void react_pickup(TickType_t *wake)
{
    camera_shot(MUSE_SHOT_MEDIUM, 350);
    look(MUSE_GAZE_TARGET, 0, 0);
    g_muse.blink_request = true;
    g_muse.expr_override = MUSE_EXPR_HAPPY;
    breathe_for(1100, busy, wake);
}

// ---------------------------------------------------------------- idle life
// Now and then Muse does something by itself, quietly.
static void fidget(TickType_t *wake)
{
    const int mid = s_lean->count / 2;
    const uint32_t r = esp_random() % 100;
    if (r < 40) {
        // Comes right up to the glass for a look at you.
        camera_shot(MUSE_SHOT_PEEK, 600);
        look(MUSE_GAZE_TARGET, 0, 0);
        g_muse.expr_override = MUSE_EXPR_SURPRISED;
        if (!breathe_for(800, busy, wake)) {
            return;
        }
        g_muse.expr_override = MUSE_EXPR_NORMAL;
        g_muse.blink_request = true;
        if (!breathe_for(900, busy, wake)) {
            return;
        }
        g_muse.blink_request = true;
        breathe_for(700, busy, wake);
    } else if (r < 75) {
        // Looks around: one way, then the other.
        const int side = (esp_random() & 1) ? 1 : -1;
        int lean = mid;
        for (int pass = 0; pass < 2; pass++) {
            const int to = mid + (pass == 0 ? side : -side);
            look(MUSE_GAZE_TARGET, pass == 0 ? side : -side, -0.1f);
            for (int i = 0; i < 9; i++) {
                if (busy()) {
                    return;
                }
                lean_toward(&lean, to);
                tick(wake, 10);
            }
        }
        look(MUSE_GAZE_TARGET, 0, 0);
        while (lean != mid) {
            lean_toward(&lean, mid);
            tick(wake, 10);
        }
    } else if (r < 90) {
        // A happy wiggle.
        g_muse.expr_override = MUSE_EXPR_HAPPY;
        play_all(s_giggle, wake);
    } else {
        // A stretch.
        if (play_all(s_squeeze, wake)) {
            play(s_squeeze, s_squeeze->count - 1, 0, 0, wake);
        }
    }
}

static bool s_sound_asleep;  // told to sleep: being carried about doesn't wake Muse, a touch does

static bool waking(void)
{
    return busy() || (g_muse.moved && !s_sound_asleep) || g_muse.voice_active ||
           g_muse.claude_mood >= MUSE_MOOD_THINKING;
}

// After a quiet while Muse dozes off: heavy eyelids, then eyes closed, slow
// breathing and "z z Z", a step closer, the screen dimmed. Touching it,
// picking it up, pressing BOOT or anything from Claude Code wakes it.
static void doze(TickType_t *wake)
{
    face_set_caption("");
    look(MUSE_GAZE_TARGET, 0, 0.6f);
    g_muse.blink_request = true;
    if (!breathe_for(900, waking, wake)) {
        return;
    }
    g_muse.blink_request = true;
    if (!breathe_for(700, waking, wake)) {
        return;
    }
    ESP_LOGI(TAG, "dozing off");
    g_muse.expr_override = MUSE_EXPR_SLEEP;
    camera_shot(MUSE_SHOT_MEDIUM, 2500);
    int level = panel_awake_brightness();
    while (!waking()) {
        if (level > SLEEP_BRIGHTNESS) {
            level = level - 4 > SLEEP_BRIGHTNESS ? level - 4 : SLEEP_BRIGHTNESS;
            panel_brightness(level);
        }
        breathe(wake, SLEEP_FPS);
    }
    ESP_LOGI(TAG, "awake");
    panel_brightness(panel_awake_brightness());
    if (g_muse.touch_down || g_muse.moved) {
        companion_report_awake();  // someone is here
    }
    g_muse.moved = false;
    muse_event_t next;
    if (g_muse.shake || speaking_up() || (xQueuePeek(s_events, &next, 0) == pdTRUE && next == MUSE_EV_TALK)) {
        g_muse.expr_override = MUSE_EXPR_NORMAL;
        return;  // straight to it
    }
    // Wide-eyed for a moment, a blink, "mm?".
    g_muse.expr_override = MUSE_EXPR_SURPRISED;
    look(MUSE_GAZE_TARGET, 0, 0);
    audio_play(MUSE_SOUND_LISTEN);
    breathe_for(400, NULL, wake);
    g_muse.expr_override = MUSE_EXPR_NORMAL;
    g_muse.blink_request = true;
    breathe_for(300, NULL, wake);
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
    case COMPANION_ALERT:
        // A timer is up: its label on a sign, two chirps and a wave.
        face_set_caption("");
        face_show_sign(e->caption[0] ? e->caption : "Time's up!", 15000);
        camera_shot(MUSE_SHOT_MEDIUM, 300);
        audio_play(MUSE_SOUND_CHEER);
        audio_play(MUSE_SOUND_HELLO);
        g_muse.expr_override = MUSE_EXPR_HAPPY;
        play_all(s_wave, wake);
        audio_play(MUSE_SOUND_CHEER);
        linger_ms = 0;
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

// Claude is waiting for your permission: come right up to the glass, then
// step back and wave to get your attention.
static void react_nudge(TickType_t *wake)
{
    char caption[COMPANION_CAPTION_LEN];
    companion_mood_caption(caption, sizeof(caption));
    face_set_caption(caption);
    chirp(MUSE_SOUND_WHOOSH);
    camera_shot(MUSE_SHOT_PEEK, 350);
    g_muse.expr_override = MUSE_EXPR_SURPRISED;
    look(MUSE_GAZE_TARGET, 0, 0);
    if (!breathe_for(900, NULL, wake)) {
        return;
    }
    camera_shot(MUSE_SHOT_MEDIUM, 350);
    play_all(s_wave, wake);
}

// Where Muse looks while Claude works, and the caption under Muse.
static void follow_claude(void)
{
    const int64_t now = muse_now_ms();
    const int mood = g_muse.claude_mood;
    switch (mood) {
    case MUSE_MOOD_THINKING:
        look(MUSE_GAZE_TARGET, (now / 2600) % 3 ? 0.6f : 0.0f, (now / 2600) % 3 ? -0.9f : 0.0f);  // up at the cloud
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
    g_muse.thought = mood == MUSE_MOOD_THINKING;  // Claude is thinking: so is Muse
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
        camera_shot(MUSE_SHOT_MEDIUM, 300);  // a step toward you
        audio_play(MUSE_SOUND_HELLO);
        play_all(s_wave, wake);
        break;
    case MUSE_EV_BELLY:
        camera_shot(MUSE_SHOT_MEDIUM, 300);
        audio_play(MUSE_SOUND_GIGGLE);
        play_all(s_giggle, wake);
        break;
    case MUSE_EV_FACE:
        react_boop(wake);
        break;
    case MUSE_EV_LONG_PRESS:
        react_squeeze(wake);
        break;
    case MUSE_EV_SWIPE_LEFT:
    case MUSE_EV_SWIPE_RIGHT:
        react_swipe(ev == MUSE_EV_SWIPE_RIGHT, wake);
        break;
    case MUSE_EV_TALK:
        react_talk(wake, true);
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
    active();

    const int lean_mid = s_lean->count / 2;
    int lean = lean_mid;
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
            active();
            continue;
        }
        if (g_muse.shake) {
            react_dizzy(&wake);
            g_muse.moved = false;
            settle();
            active();
            continue;
        }
        if (s_sleep_asked) {
            // The AI said good night: nod off now. (Talking set BOOT down; let go first.)
            s_sleep_asked = false;
            s_sound_asleep = true;
            doze(&wake);
            s_sound_asleep = false;
            rest();
            active();
            continue;
        }
        if (g_muse.petting) {
            react_pet(&wake);
            settle();
            active();
            continue;
        }
        muse_event_t ev;
        if (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
            react(ev, &wake);
            settle();
            active();
            continue;
        }
        if (speaking_up()) {
            react_talk(&wake, false);  // Muse has something to say
            settle();
            active();
            continue;
        }
        companion_event_t claude;
        if (companion_next_event(&claude)) {
            react_claude(&claude, &wake);
            settle();
            active();
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
                active();
                continue;
            }
        }
        last_mood = mood;
        g_muse.agent_error = false;  // errors outside a conversation need no reaction
        if (g_muse.voice_active && !g_muse.touch_down) {
            react_listen(&wake);
            settle();
            active();
            continue;
        }
        if (g_muse.touch_down || g_muse.button_down || mood >= MUSE_MOOD_THINKING) {
            active();
        }

        // Picked up or moved: Muse stays awake, and greets you if it had been resting.
        if (g_muse.moved) {
            g_muse.moved = false;
            const bool was_resting = muse_now_ms() - s_active_ms > PICKUP_REST_MS;
            active();
            if (was_resting && !g_muse.touch_down) {
                companion_report_awake();
                react_pickup(&wake);
                rest();
                continue;
            }
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
            } else {
                clips_refresh();
            }
            tick(&wake, LEAN_FPS);
            if (lean == lean_mid && !leaning) {
                s_idle_pos = 0;
            }
            continue;
        }

        // Nothing is happening. After a quiet while Muse dozes off; before
        // that, now and then, it fidgets.
        const int64_t now = muse_now_ms();
        const bool undisturbed = mood <= MUSE_MOOD_IDLE && !g_muse.touch_down && now >= s_caption_until_ms;
        if (SLEEP_AFTER_MS > 0 && undisturbed && now - s_active_ms > SLEEP_AFTER_MS) {
            doze(&wake);
            rest();
            active();
            continue;
        }
        if (FIDGETS && undisturbed && now >= s_next_fidget_ms) {
            fidget(&wake);
            rest();
            s_next_fidget_ms = muse_now_ms() + FIDGET_MIN_MS + esp_random() % FIDGET_SPAN_MS;
            continue;
        }

        follow_claude();
        breathe(&wake, 0);
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
    // Where the face rests: the middle of the eyes over the idle loop.
    float sum = 0;
    for (int i = 0; i < s_idle->count; i++) {
        const muse_anchor_t *a = clips_anchor(s_idle->first + i);
        sum += (a->eye_l_x + a->eye_r_x) * 0.125f;
    }
    s_home_x = sum / s_idle->count;
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
