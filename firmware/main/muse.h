// SPDX-License-Identifier: MIT
// Shared types and state for the Muse charm firmware.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_timer.h"

// Facial expressions. 0..3 can come from the clip data (set per frame in
// Blender); the rest are chosen by the firmware.
typedef enum {
    MUSE_EXPR_NORMAL = 0,     // dot eyes, small smile
    MUSE_EXPR_HAPPY = 1,      // ^ ^ eyes, open mouth
    MUSE_EXPR_SQUEEZE = 2,    // > < eyes, open mouth
    MUSE_EXPR_DIZZY = 3,      // spinning eyes, small round mouth
    MUSE_EXPR_TALK = 4,       // dot eyes, mouth follows the microphone level
    MUSE_EXPR_SURPRISED = 5,  // wide eyes, small round mouth
    MUSE_EXPR_SLEEP = 6,      // closed eyes, small smile, "z z Z"
} muse_expr_t;

typedef enum {
    MUSE_GAZE_AUTO = 0,    // idle glances chosen by the face
    MUSE_GAZE_TARGET = 1,  // look where behaviour says
} muse_gaze_mode_t;

// Inputs that trigger a reaction.
typedef enum {
    MUSE_EV_TAP,
    MUSE_EV_BELLY,
    MUSE_EV_LONG_PRESS,
    MUSE_EV_SWIPE_LEFT,
    MUSE_EV_SWIPE_RIGHT,
    MUSE_EV_FACE,          // tap on the face: a boop
    MUSE_EV_TALK,          // BOOT held with voice chat on: start a voice chat turn
    MUSE_EV_BUTTON,        // BOOT pressed without voice chat: squeeze
    MUSE_EV_SETUP,         // open setup (screen held 5 s, or BOOT 3 s without voice chat)
} muse_event_t;

// Voice chat progress (agent.c).
typedef enum {
    MUSE_AGENT_OFF = 0,     // not available: not linked, or no Deepgram key on the computer
    MUSE_AGENT_IDLE,
    MUSE_AGENT_CONNECTING,
    MUSE_AGENT_LISTENING,
    MUSE_AGENT_THINKING,
    MUSE_AGENT_SPEAKING,
} muse_agent_state_t;

// What Claude Code is up to on the computer (companion.c).
typedef enum {
    MUSE_MOOD_OFF = 0,      // not linked to the companion server
    MUSE_MOOD_IDLE,
    MUSE_MOOD_THINKING,
    MUSE_MOOD_WORKING,      // running a tool
    MUSE_MOOD_NEEDS_YOU,    // waiting for your permission
} muse_mood_t;

// What the AI can make Muse do (its muse_do tool; agent.c knows the names).
typedef enum {
    MUSE_MOVE_NONE = -1,
    MUSE_MOVE_WAVE = 0,
    MUSE_MOVE_GIGGLE,
    MUSE_MOVE_CHEER,
    MUSE_MOVE_DANCE,
    MUSE_MOVE_SPIN,
    MUSE_MOVE_SHAKE_HEAD,
    MUSE_MOVE_TILT_HEAD,
    MUSE_MOVE_BOUNCE,
    MUSE_MOVE_SWAY,
    MUSE_MOVE_SMILE,        // a face for a moment, not a body move
    MUSE_MOVE_SURPRISED,
    MUSE_MOVE_COME_CLOSER,  // the camera, for the rest of the reply
    MUSE_MOVE_STEP_BACK,
    MUSE_MOVE_SLEEP,        // doze off once the reply is over
    MUSE_MOVE_COUNT,
} muse_move_t;

// State shared between tasks. Each field has one writer and is 32 bits, so
// plain volatile access is enough.
typedef struct {
    volatile int frame;            // frame index on screen (clips)
    volatile int expr_override;    // muse_expr_t, or -1 to use the frame's own (behaviour)
    volatile int gaze_mode;        // muse_gaze_mode_t (behaviour)
    volatile float gaze_x;         // -1..1 (behaviour)
    volatile float gaze_y;         // -1..1, negative is up (behaviour)
    volatile bool blink_request;   // (behaviour, cleared by face)

    volatile bool touch_down;      // (touch)
    volatile int touch_x;          // on the screen
    volatile int touch_y;
    volatile int touch_fx;         // the same point in the body frame, through the camera
    volatile int touch_fy;
    volatile bool petting;         // the finger is stroking Muse's head (touch)

    volatile float tilt_deg;       // + means Muse should lean clockwise (motion)
    volatile float downhill;       // -1..1, which side of the screen is lower (motion)
    volatile bool shake;           // set by motion, cleared by behaviour
    volatile bool moved;           // picked up or moved; set by motion, cleared by behaviour
    volatile bool flat;            // lying on its back or its face (motion)

    volatile bool voice_active;    // (audio)
    volatile float mouth_level;    // 0..1, from the voice or Muse's own reply (audio)
    volatile float mic_level;      // 0..1, how loud you are while Muse listens (audio)
    volatile int noise_db;         // how noisy the room is, roughly 25 (silent) to 75 (loud) (audio)
    volatile bool button_down;     // BOOT button (touch)

    volatile int agent_state;      // muse_agent_state_t (agent)
    volatile bool thought;         // show the thought cloud over Muse's head (behaviour)
    volatile bool agent_error;     // set by agent, cleared by behaviour
    volatile bool setup_active;    // setup portal open (wifi)
    volatile bool setup_request;   // open on-screen setup (wifi -> behaviour)
    volatile int claude_mood;      // muse_mood_t (companion)
} muse_state_t;

extern muse_state_t g_muse;

static inline float muse_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int64_t muse_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}
