// SPDX-License-Identifier: MIT
// Voice chat, relayed by the companion server (companion/muse_voice.py).
//
//   hold BOOT        -> {"type":"talk_start"}, then microphone audio (binary,
//                       16 kHz 16-bit mono, starting with a short pre-roll)
//   let go           -> {"type":"talk_end"}
//   hold again       -> {"type":"talk_stop"} to stop the reply, then a new turn
//   server           -> {"type":"voice_ready","on":true}  a Deepgram key is set up
//                       {"type":"voice","state":"thinking|speaking|done|error"}
//                       {"type":"voice","action":"wave"}  an animation to play
//                       {"type":"voice","heard":"..."}   what you said
//                       {"type":"voice","say":"...","cps":17}  a sentence of the reply
//                       reply audio (binary), paced a little ahead of playback
//
// WiFi stays in modem sleep during a conversation: turning power saving off
// draws more current, and on this board that dipped the supply (glitches on
// GPIO0, the panel going dark). Larger TCP buffers keep the audio flowing.

#include "agent.h"

#include <stdio.h>
#include <string.h>

#include "companion.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "muse.h"

static const char *TAG = "agent";

#define MIC_BUFFER_BYTES (128 * 1024)     // ~4 s
#define SPEECH_BUFFER_BYTES (256 * 1024)  // ~8 s; the server sends at most ~2.5 s ahead
#define SEND_CHUNK 1280                   // 40 ms
#define REPLY_TIMEOUT_MS 20000            // nothing back after release: give up
#define SPEECH_DONE_GAP_MS 300
#define BYTES_PER_MS 32                   // 16 kHz, 16-bit
#define SUB_LINE 26                       // characters that fit on the caption line
#define SUB_LINGER_MS 1500                // a subtitle stays this long after its sentence
#define DEFAULT_CPS 16                    // speaking rate, characters per second

typedef struct {
    int64_t at_ms;  // when its audio starts playing
    int cps;
    char text[200];
} sentence_t;

static StreamBufferHandle_t s_mic, s_speech;
static StaticStreamBuffer_t s_mic_ctrl, s_speech_ctrl;

static volatile bool s_available;  // linked, and the server has a Deepgram key
static volatile bool s_turn_active, s_discard, s_audio_done;
static volatile bool s_start_due, s_end_due, s_stop_due;
static volatile int64_t s_turn_end_ms;

// Subtitles: sentences of the reply, shown as their audio plays.
static QueueHandle_t s_sentences;
static SemaphoreHandle_t s_text_lock;
static char s_heard[120];
static sentence_t s_current;      // behaviour task only
static int64_t s_current_start;   // behaviour task only

// Sending statistics for the log.
static int s_sent_bytes;
static int64_t s_slowest_send_ms;

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
            s_sent_bytes = 0;
            s_slowest_send_ms = 0;
            companion_send_text("{\"type\":\"talk_start\"}");
        }
        size_t n;
        while ((n = xStreamBufferReceive(s_mic, chunk, sizeof(chunk), 0)) > 0) {
            const int64_t t0 = muse_now_ms();
            if (companion_send_binary(chunk, n)) {
                s_sent_bytes += n;
            }
            const int64_t took = muse_now_ms() - t0;
            if (took > s_slowest_send_ms) {
                s_slowest_send_ms = took;
            }
        }
        if (s_end_due && !s_turn_active) {
            s_end_due = false;
            companion_send_text("{\"type\":\"talk_end\"}");
            ESP_LOGI(TAG, "sent %.1f s of audio (slowest send %lld ms)", s_sent_bytes / (BYTES_PER_MS * 1000.0f),
                     (long long)s_slowest_send_ms);
        }

        // The reply is finished once the server says so and playback has drained.
        const int state = g_muse.agent_state;
        if (state == MUSE_AGENT_SPEAKING) {
            if (s_audio_done && xStreamBufferIsEmpty(s_speech)) {
                if (!speech_empty_since) {
                    speech_empty_since = now;
                } else if (now - speech_empty_since > SPEECH_DONE_GAP_MS) {
                    set_state(MUSE_AGENT_IDLE);
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

void agent_on_voice(const char *state, const char *action, const char *error, const char *heard, const char *say,
                    int cps)
{
    if (heard && s_text_lock) {
        xSemaphoreTake(s_text_lock, portMAX_DELAY);
        snprintf(s_heard, sizeof(s_heard), "%s", heard);
        xSemaphoreGive(s_text_lock);
        ESP_LOGI(TAG, "heard: %s", heard);
    }
    if (say && s_sentences && !s_turn_active) {
        // It plays after the reply audio already waiting in the buffer.
        sentence_t sentence = {
            .at_ms = muse_now_ms() + (int64_t)xStreamBufferBytesAvailable(s_speech) / BYTES_PER_MS,
            .cps = cps > 0 ? cps : DEFAULT_CPS,
        };
        snprintf(sentence.text, sizeof(sentence.text), "%s", say);
        xQueueSend(s_sentences, &sentence, 0);
    }
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
    s_sentences = xQueueCreate(8, sizeof(sentence_t));
    s_text_lock = xSemaphoreCreateMutex();
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
    sentence_t old;
    while (s_sentences && xQueueReceive(s_sentences, &old, 0) == pdTRUE) {
    }
    s_current.text[0] = '\0';
    xSemaphoreTake(s_text_lock, portMAX_DELAY);
    s_heard[0] = '\0';
    xSemaphoreGive(s_text_lock);
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

// ---------------------------------------------------------------- captions
// The part of `text` to show `chars` characters into it, one caption line at a
// time, broken between words.
static void line_at(const char *text, int chars, char *buf, size_t len)
{
    const int n = (int)strlen(text);
    int start = 0;
    for (;;) {
        int end = start + SUB_LINE;
        if (end >= n) {
            end = n;
        } else {
            int space = end;
            while (space > start && text[space] != ' ') {
                space--;
            }
            if (space > start) {
                end = space;
            }
        }
        if (end >= n || chars < end) {
            snprintf(buf, len, "%.*s", end - start, text + start);
            return;
        }
        start = end;
        while (text[start] == ' ') {
            start++;
        }
    }
}

void agent_caption(char *buf, size_t len)
{
    const int64_t now = muse_now_ms();
    sentence_t next;
    while (s_sentences && xQueuePeek(s_sentences, &next, 0) == pdTRUE && next.at_ms <= now) {
        xQueueReceive(s_sentences, &s_current, 0);
        s_current_start = now;
    }
    buf[0] = '\0';
    const int state = g_muse.agent_state;
    if (state == MUSE_AGENT_LISTENING || state == MUSE_AGENT_CONNECTING) {
        snprintf(buf, len, "Listening...");
        return;
    }
    if (s_current.text[0]) {
        const int chars = (int)((now - s_current_start) * s_current.cps / 1000);
        const int total = (int)strlen(s_current.text);
        if (chars < total + s_current.cps * SUB_LINGER_MS / 1000) {
            line_at(s_current.text, chars, buf, len);
            return;
        }
        s_current.text[0] = '\0';
    }
    if (state == MUSE_AGENT_THINKING && s_text_lock) {
        xSemaphoreTake(s_text_lock, portMAX_DELAY);
        if (s_heard[0]) {
            char quoted[sizeof(s_heard) + 4];
            snprintf(quoted, sizeof(quoted), "\"%s\"", s_heard);
            line_at(quoted, 0, buf, len);  // the start of what you said
        } else {
            snprintf(buf, len, "Hmm...");
        }
        xSemaphoreGive(s_text_lock);
    }
}
