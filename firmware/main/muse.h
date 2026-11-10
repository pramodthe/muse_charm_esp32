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
    MUSE_EV_TALK,          // screen held still: start a voice chat turn
    MUSE_EV_BUTTON,        // BOOT button pressed: squeeze
    MUSE_EV_SETUP,         // BOOT button held 3 s: open the setup page
} muse_event_t;

// Voice chat progress (agent.c).
typedef enum {
    MUSE_AGENT_OFF = 0,     // no Deepgram key saved
    MUSE_AGENT_IDLE,
    MUSE_AGENT_CONNECTING,
    MUSE_AGENT_LISTENING,
    MUSE_AGENT_THINKING,
    MUSE_AGENT_SPEAKING,
} muse_agent_state_t;

// Animations the AI can ask for while it talks.
typedef enum {
    MUSE_ACTION_NONE = -1,
    MUSE_ACTION_WAVE = 0,
    MUSE_ACTION_GIGGLE,
    MUSE_ACTION_CHEER,
    MUSE_ACTION_DIZZY,
} muse_action_t;

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
    volatile int touch_x;
    volatile int touch_y;

    volatile float tilt_deg;       // + means Muse should lean clockwise (motion)
    volatile float downhill;       // -1..1, which side of the screen is lower (motion)
    volatile bool shake;           // set by motion, cleared by behaviour

    volatile bool voice_active;    // (audio)
    volatile float mouth_level;    // 0..1, from the voice or Muse's own reply (audio)
    volatile bool button_down;     // BOOT button (touch)

    volatile int agent_state;      // muse_agent_state_t (agent)
    volatile int agent_action;     // muse_action_t requested by the AI, cleared by behaviour
    volatile bool agent_error;     // set by agent, cleared by behaviour
    volatile bool setup_active;    // setup portal open (wifi)
    volatile bool setup_request;   // open on-screen setup (wifi -> behaviour)
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
