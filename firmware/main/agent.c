// SPDX-License-Identifier: MIT
// Voice chat, relayed by the companion server (companion/muse_voice.py).
//
//   hold the screen  -> {"type":"talk_start"}, then microphone audio (binary,
//                       16 kHz 16-bit mono, starting with a short pre-roll)
//   let go           -> {"type":"talk_end"}
//   hold again       -> {"type":"talk_stop"} to stop the reply, then a new turn
//   server           -> {"type":"voice_ready","on":true}  a Deepgram key is set up
//                       {"type":"voice","state":"thinking|speaking|done|error"}
//                       {"type":"voice","action":"wave"}  an animation to play
//                       reply audio (binary), paced a little ahead of playback

#include "agent.h"

#include <string.h>

#include "companion.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "muse.h"
#include "wifi.h"

static const char *TAG = "agent";

#define MIC_BUFFER_BYTES (128 * 1024)     // ~4 s
#define SPEECH_BUFFER_BYTES (256 * 1024)  // ~8 s; the server sends at most ~2.5 s ahead
#define SEND_CHUNK 1280                   // 40 ms
#define REPLY_TIMEOUT_MS 20000            // nothing back after release: give up
#define SPEECH_DONE_GAP_MS 300

static StreamBufferHandle_t s_mic, s_speech;
static StaticStreamBuffer_t s_mic_ctrl, s_speech_ctrl;

static volatile bool s_available;  // linked, and the server has a Deepgram key
static volatile bool s_turn_active, s_discard, s_audio_done;
static volatile bool s_start_due, s_end_due, s_stop_due;
static volatile int64_t s_turn_end_ms;

static const char *const ACTIONS[] = {"wave", "giggle", "cheer", "dizzy"};

static void set_state(muse_agent_state_t state)
{
    if (g_muse.agent_state != (int)state) {
        static const char *const names[] = {"off", "idle", "connecting", "listening", "thinking", "speaking"};
        ESP_LOGI(TAG, "%s", names[state]);
        g_muse.agent_state = state;
    }
}

static void fail(const char *why)
{
    ESP_LOGW(TAG, "%s", why);
    s_turn_active = false;
    s_discard = true;
    g_muse.agent_error = true;
    set_state(s_available ? MUSE_AGENT_IDLE : MUSE_AGENT_OFF);
    wifi_low_latency(false);
}

static void agent_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[SEND_CHUNK];
    int64_t speech_empty_since = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        const int64_t now = muse_now_ms();

        // Messages and microphone audio to the server, in order.
        if (s_stop_due) {
            s_stop_due = false;
            companion_send_text("{\"type\":\"talk_stop\"}");
        }
        if (s_start_due) {
            s_start_due = false;
            companion_send_text("{\"type\":\"talk_start\"}");
        }
        size_t n;
        while ((n = xStreamBufferReceive(s_mic, chunk, sizeof(chunk), 0)) > 0) {
            companion_send_binary(chunk, n);
        }
        if (s_end_due && !s_turn_active) {
            s_end_due = false;
            companion_send_text("{\"type\":\"talk_end\"}");
        }

        // The reply is finished once the server says so and playback has drained.
        const int state = g_muse.agent_state;
        if (state == MUSE_AGENT_SPEAKING) {
            if (s_audio_done && xStreamBufferIsEmpty(s_speech)) {
                if (!speech_empty_since) {
                    speech_empty_since = now;
                } else if (now - speech_empty_since > SPEECH_DONE_GAP_MS) {
                    set_state(MUSE_AGENT_IDLE);
                    wifi_low_latency(false);
                }
            } else {
                speech_empty_since = 0;
            }
        } else if (state == MUSE_AGENT_THINKING && now - s_turn_end_ms > REPLY_TIMEOUT_MS) {
            fail("no reply from the computer");
        }
    }
}

// ---------------------------------------------------------------- from companion.c
void agent_on_link_lost(void)
{
    const bool busy = s_turn_active || g_muse.agent_state >= MUSE_AGENT_CONNECTING;
    s_available = false;
    if (busy) {
        fail("lost the link to the computer");
    } else {
        set_state(MUSE_AGENT_OFF);
    }
}

void agent_on_voice_ready(bool on)
{
    s_available = on;
    const int state = g_muse.agent_state;
    if (state == MUSE_AGENT_OFF || state == MUSE_AGENT_IDLE) {
        set_state(on ? MUSE_AGENT_IDLE : MUSE_AGENT_OFF);
    }
    ESP_LOGI(TAG, "voice chat %s", on ? "ready: hold the screen to talk"
                                      : "off: put a Deepgram key on the computer (see companion/README.md)");
}

void agent_on_voice(const char *state, const char *action, const char *error)
{
    if (action) {
        for (int i = 0; i < 4; i++) {
            if (strcmp(action, ACTIONS[i]) == 0) {
                g_muse.agent_action = i;
                ESP_LOGI(TAG, "action: %s", action);
            }
        }
    }
    if (state && strcmp(state, "error") == 0) {
        ESP_LOGW(TAG, "the computer says: %s", error ? error : "error");
        fail("voice chat failed");
        return;
    }
    if (!state || s_turn_active) {
        return;
    }
    if (strcmp(state, "thinking") == 0) {
        if (g_muse.agent_state != MUSE_AGENT_SPEAKING) {
            set_state(MUSE_AGENT_THINKING);
        }
    } else if (strcmp(state, "speaking") == 0) {
        s_discard = false;
        s_audio_done = false;
        set_state(MUSE_AGENT_SPEAKING);
    } else if (strcmp(state, "done") == 0) {
        s_audio_done = true;
    }
}

void agent_on_audio(const void *data, int len)
{
    if (s_speech && !s_discard && !s_turn_active && len > 0) {
        xStreamBufferSend(s_speech, data, len, pdMS_TO_TICKS(200));
    }
}

// ---------------------------------------------------------------- public API
void agent_init(void)
{
    uint8_t *mic = heap_caps_malloc(MIC_BUFFER_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *speech = heap_caps_malloc(SPEECH_BUFFER_BYTES, MALLOC_CAP_SPIRAM);
    if (!mic || !speech) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    s_mic = xStreamBufferCreateStatic(MIC_BUFFER_BYTES, 1, mic, &s_mic_ctrl);
    s_speech = xStreamBufferCreateStatic(SPEECH_BUFFER_BYTES, 1, speech, &s_speech_ctrl);
    g_muse.agent_action = MUSE_ACTION_NONE;
    set_state(MUSE_AGENT_OFF);
    xTaskCreatePinnedToCore(agent_task, "muse_agent", 4 * 1024, NULL, 4, NULL, 0);
}

bool agent_configured(void)
{
    return s_available;
}

bool agent_begin_turn(void)
{
    if (!s_mic) {
        return false;
    }
    if (!s_available) {
        fail(companion_connected() ? "the computer has no Deepgram key" : "not linked to the computer");
        return false;
    }
    s_discard = true;  // stop any reply that is still playing
    s_end_due = false;
    s_turn_active = true;
    s_start_due = true;
    wifi_low_latency(true);
    set_state(MUSE_AGENT_LISTENING);
    return true;
}

void agent_end_turn(void)
{
    if (!s_turn_active) {
        return;
    }
    s_turn_active = false;
    s_end_due = true;
    s_turn_end_ms = muse_now_ms();
    s_audio_done = false;
    set_state(MUSE_AGENT_THINKING);
}

void agent_stop_speaking(void)
{
    s_discard = true;
    s_stop_due = true;
    if (g_muse.agent_state == MUSE_AGENT_SPEAKING) {
        set_state(MUSE_AGENT_IDLE);
    }
}

bool agent_listening(void)
{
    return s_turn_active;
}

void agent_push_mic(const int16_t *pcm, int samples)
{
    if (s_mic && s_turn_active) {
        xStreamBufferSend(s_mic, pcm, samples * sizeof(int16_t), 0);
    }
}

int agent_pull_speech(int16_t *pcm, int samples)
{
    if (!s_speech) {
        return 0;
    }
    if (s_discard || s_turn_active) {
        // Throw away whatever is buffered (barge-in or a stale reply).
        uint8_t scratch[640];
        while (xStreamBufferReceive(s_speech, scratch, sizeof(scratch), 0) > 0) {
        }
        return 0;
    }
    return (int)(xStreamBufferReceive(s_speech, pcm, samples * sizeof(int16_t), 0) / sizeof(int16_t));
}
