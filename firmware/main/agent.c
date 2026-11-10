// SPDX-License-Identifier: MIT
// Voice chat through the Deepgram Voice Agent API.
//
//   wss://agent.deepgram.com/v1/agent/converse  (Authorization: Token <key>)
//   -> Settings (16 kHz linear16 in and out, listen / think / speak providers,
//      Muse's personality, the muse_action function, recent history)
//   <- SettingsApplied
//   -> microphone audio as binary frames while the screen is held, then a
//      little silence so Deepgram notices the end of the turn
//   <- AgentThinking, ConversationText, AgentStartedSpeaking,
//      reply audio as binary frames, AgentAudioDone
//   <- FunctionCallRequest muse_action -> Muse animates -> FunctionCallResponse
//
// The connection opens on the first press, stays open between turns with
// KeepAlive, and closes after a quiet spell. The websocket plumbing follows
// soniox_client / soniox_tts_client in esp32-agui (crt bundle, PSRAM stream
// buffers, back-pressure on incoming audio).

#include "agent.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "muse.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "agent";

#define AGENT_URI "wss://agent.deepgram.com/v1/agent/converse"
#define SAMPLE_RATE 16000
#define MIC_BUFFER_BYTES (128 * 1024)     // ~4 s, covers the connection handshake
#define SPEECH_BUFFER_BYTES (512 * 1024)  // ~16 s of reply audio
#define SEND_CHUNK 1280                   // 40 ms
#define TAIL_SILENCE_MS 1200              // after release, so Deepgram ends the turn
#define KEEPALIVE_MS 7000                 // Deepgram asks for one every 8 s when idle
#define CONNECT_TIMEOUT_MS 10000
#define REPLY_TIMEOUT_MS 15000            // nothing back after release: give up
#define SPEECH_DONE_GAP_MS 300
#define HISTORY_MAX 8
#define TEXT_MAX (16 * 1024)

#ifdef CONFIG_MUSE_AGENT_IDLE_CLOSE_S
#define IDLE_CLOSE_MS (CONFIG_MUSE_AGENT_IDLE_CLOSE_S * 1000)
#else
#define IDLE_CLOSE_MS 120000
#endif

typedef struct {
    char role[12];
    char content[240];
} history_t;

static esp_websocket_client_handle_t s_ws;
static StreamBufferHandle_t s_mic, s_speech;
static StaticStreamBuffer_t s_mic_ctrl, s_speech_ctrl;
static SemaphoreHandle_t s_send_lock;
static char s_auth_header[SETTING_MAX_LEN + 32];

static volatile bool s_want_connect, s_connected, s_ready, s_settings_sent, s_closed, s_failed;
static volatile bool s_turn_active, s_discard, s_audio_done;
static volatile int s_silence_ms;
static volatile int64_t s_connect_start_ms, s_turn_end_ms, s_last_send_ms, s_last_activity_ms;

// Messages from the websocket task for the agent task to send.
static char s_fn_id[96], s_fn_name[32];
static volatile bool s_fn_pending;

static history_t s_history[HISTORY_MAX];
static int s_history_count;

static char *s_text;  // reassembly of one text message (PSRAM)
static size_t s_text_len;
static int s_last_opcode;

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
    s_failed = true;
    g_muse.agent_error = true;
}

// ---------------------------------------------------------------- history
static void history_add(const char *role, const char *content)
{
    if (s_history_count == HISTORY_MAX) {
        memmove(&s_history[0], &s_history[1], sizeof(s_history[0]) * (HISTORY_MAX - 1));
        s_history_count--;
    }
    history_t *h = &s_history[s_history_count++];
    strlcpy(h->role, role, sizeof(h->role));
    strlcpy(h->content, content, sizeof(h->content));
}

// ---------------------------------------------------------------- sending
static bool send_text(const char *s)
{
    if (!s_ws || !s_connected) {
        return false;
    }
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    const int n = esp_websocket_client_send_text(s_ws, s, strlen(s), pdMS_TO_TICKS(2000));
    xSemaphoreGive(s_send_lock);
    s_last_send_ms = muse_now_ms();
    return n >= 0;
}

static bool send_json(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        return false;
    }
    const bool ok = send_text(s);
    cJSON_free(s);
    return ok;
}

static bool send_audio(const void *data, size_t len)
{
    if (!s_ws || !s_connected) {
        return false;
    }
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    const int n = esp_websocket_client_send_bin(s_ws, data, len, pdMS_TO_TICKS(500));
    xSemaphoreGive(s_send_lock);
    s_last_send_ms = muse_now_ms();
    return n >= 0;
}

static cJSON *provider(const char *type, const char *model)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "model", model);
    return p;
}

static void send_settings(void)
{
    static char personality[SETTING_MAX_LEN];
    char voice[96];
    if (!settings_get(SETTING_PERSONALITY, personality, sizeof(personality))) {
        strlcpy(personality, CONFIG_MUSE_AGENT_PROMPT, sizeof(personality));
    }
    if (!settings_get(SETTING_VOICE, voice, sizeof(voice))) {
        strlcpy(voice, CONFIG_MUSE_AGENT_DEFAULT_VOICE, sizeof(voice));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "Settings");

    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    cJSON *in = cJSON_AddObjectToObject(audio, "input");
    cJSON_AddStringToObject(in, "encoding", "linear16");
    cJSON_AddNumberToObject(in, "sample_rate", SAMPLE_RATE);
    cJSON *out = cJSON_AddObjectToObject(audio, "output");
    cJSON_AddStringToObject(out, "encoding", "linear16");
    cJSON_AddNumberToObject(out, "sample_rate", SAMPLE_RATE);
    cJSON_AddStringToObject(out, "container", "none");

    cJSON *agent = cJSON_AddObjectToObject(root, "agent");
    cJSON *listen = cJSON_AddObjectToObject(agent, "listen");
    cJSON_AddItemToObject(listen, "provider", provider("deepgram", CONFIG_MUSE_AGENT_LISTEN_MODEL));

    cJSON *think = cJSON_AddObjectToObject(agent, "think");
    cJSON_AddItemToObject(think, "provider", provider(CONFIG_MUSE_AGENT_THINK_PROVIDER, CONFIG_MUSE_AGENT_THINK_MODEL));
    cJSON_AddStringToObject(think, "prompt", personality);
    cJSON *functions = cJSON_AddArrayToObject(think, "functions");
    cJSON *fn = cJSON_CreateObject();
    cJSON_AddStringToObject(fn, "name", "muse_action");
    cJSON_AddStringToObject(fn, "description",
                            "Make Muse do a short animation on its screen while it talks. Use it when it fits: "
                            "wave for hello or goodbye, giggle at something funny, cheer at good news, dizzy "
                            "when confused.");
    cJSON *params = cJSON_AddObjectToObject(fn, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *action = cJSON_AddObjectToObject(props, "action");
    cJSON_AddStringToObject(action, "type", "string");
    cJSON_AddItemToObject(action, "enum", cJSON_CreateStringArray(ACTIONS, 4));
    cJSON_AddItemToObject(params, "required", cJSON_CreateStringArray((const char *const[]){"action"}, 1));
    cJSON_AddItemToArray(functions, fn);

    cJSON *speak = cJSON_AddObjectToObject(agent, "speak");
    cJSON_AddItemToObject(speak, "provider", provider("deepgram", voice));

    // Recent conversation, so a reconnect doesn't make Muse forget.
    if (s_history_count > 0) {
        cJSON *context = cJSON_AddObjectToObject(agent, "context");
        cJSON *messages = cJSON_AddArrayToObject(context, "messages");
        for (int i = 0; i < s_history_count; i++) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "type", "History");
            cJSON_AddStringToObject(m, "role", s_history[i].role);
            cJSON_AddStringToObject(m, "content", s_history[i].content);
            cJSON_AddItemToArray(messages, m);
        }
    }
    ESP_LOGI(TAG, "settings: voice %s, think %s/%s", voice, CONFIG_MUSE_AGENT_THINK_PROVIDER,
             CONFIG_MUSE_AGENT_THINK_MODEL);
    send_json(root);
}

// ---------------------------------------------------------------- receiving
static void handle_function_call(cJSON *root)
{
    cJSON *fns = cJSON_GetObjectItem(root, "functions");
    cJSON *f;
    cJSON_ArrayForEach(f, fns) {
        const cJSON *name = cJSON_GetObjectItem(f, "name");
        const cJSON *id = cJSON_GetObjectItem(f, "id");
        const cJSON *args = cJSON_GetObjectItem(f, "arguments");
        if (!cJSON_IsString(name) || strcmp(name->valuestring, "muse_action") != 0) {
            continue;
        }
        cJSON *parsed = cJSON_IsString(args) ? cJSON_Parse(args->valuestring) : NULL;
        const cJSON *obj = parsed ? parsed : args;
        const cJSON *action = cJSON_GetObjectItem(obj, "action");
        if (cJSON_IsString(action)) {
            for (int i = 0; i < 4; i++) {
                if (strcmp(action->valuestring, ACTIONS[i]) == 0) {
                    g_muse.agent_action = i;
                    ESP_LOGI(TAG, "action: %s", ACTIONS[i]);
                }
            }
        }
        cJSON_Delete(parsed);
        strlcpy(s_fn_id, cJSON_IsString(id) ? id->valuestring : "", sizeof(s_fn_id));
        strlcpy(s_fn_name, name->valuestring, sizeof(s_fn_name));
        s_fn_pending = true;  // the agent task sends the response
    }
}

static void handle_message(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    if (strcmp(t, "SettingsApplied") == 0) {
        s_ready = true;
        ESP_LOGI(TAG, "ready");
    } else if (strcmp(t, "UserStartedSpeaking") == 0) {
        s_discard = true;  // drop what is left of an earlier reply
    } else if (strcmp(t, "ConversationText") == 0) {
        const cJSON *role = cJSON_GetObjectItem(root, "role");
        const cJSON *content = cJSON_GetObjectItem(root, "content");
        if (cJSON_IsString(role) && cJSON_IsString(content)) {
            ESP_LOGI(TAG, "%s: %s", strcmp(role->valuestring, "user") == 0 ? "you" : "muse", content->valuestring);
            history_add(role->valuestring, content->valuestring);
        }
        s_last_activity_ms = muse_now_ms();
    } else if (strcmp(t, "AgentThinking") == 0) {
        if (!s_turn_active) {
            set_state(MUSE_AGENT_THINKING);
        }
    } else if (strcmp(t, "AgentStartedSpeaking") == 0) {
        if (!s_turn_active) {
            s_discard = false;
            s_audio_done = false;
            set_state(MUSE_AGENT_SPEAKING);
        }
    } else if (strcmp(t, "AgentAudioDone") == 0) {
        s_audio_done = true;
    } else if (strcmp(t, "FunctionCallRequest") == 0) {
        handle_function_call(root);
    } else if (strcmp(t, "Error") == 0) {
        const cJSON *d = cJSON_GetObjectItem(root, "description");
        ESP_LOGE(TAG, "Deepgram error: %s", cJSON_IsString(d) ? d->valuestring : json);
        fail("closing after an error");
    } else if (strcmp(t, "Warning") == 0) {
        const cJSON *d = cJSON_GetObjectItem(root, "description");
        ESP_LOGW(TAG, "Deepgram warning: %s", cJSON_IsString(d) ? d->valuestring : json);
    }
    cJSON_Delete(root);
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        s_connected = false;
        s_ready = false;
        s_closed = true;
        break;
    case WEBSOCKET_EVENT_ERROR:
        fail("connection error (check the Deepgram key and WiFi)");
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (!e || e->data_len <= 0) {
            break;
        }
        int op = e->op_code;
        if (op == 0x00) {
            op = s_last_opcode;  // continuation frame
        } else if (op == 0x01 || op == 0x02) {
            s_last_opcode = op;
        }
        if (op == 0x02) {
            // Reply audio. Back-pressure: when the buffer is full this waits for
            // playback, which paces Deepgram to real time.
            if (!s_discard && !s_turn_active) {
                xStreamBufferSend(s_speech, e->data_ptr, e->data_len, pdMS_TO_TICKS(1000));
            }
        } else if (op == 0x01) {
            if (e->payload_len > TEXT_MAX) {
                break;
            }
            if (e->payload_offset == 0) {
                s_text_len = e->payload_len;
            }
            if (e->payload_offset + e->data_len <= s_text_len) {
                memcpy(s_text + e->payload_offset, e->data_ptr, e->data_len);
            }
            if (e->payload_offset + e->data_len >= s_text_len) {
                s_text[s_text_len] = '\0';
                handle_message(s_text);
            }
        }
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------- connection
static void open_ws(void)
{
    char key[SETTING_MAX_LEN];
    if (!settings_get(SETTING_DEEPGRAM_KEY, key, sizeof(key))) {
        fail("no Deepgram key saved");
        return;
    }
    snprintf(s_auth_header, sizeof(s_auth_header), "Authorization: Token %s\r\n", key);
    const esp_websocket_client_config_t cfg = {
        .uri = AGENT_URI,
        .headers = s_auth_header,
        .buffer_size = 4096,
        .task_stack = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .network_timeout_ms = 10000,
        .disable_auto_reconnect = true,
        .ping_interval_sec = 10,
    };
    s_connected = s_ready = s_settings_sent = s_closed = false;
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        fail("could not create the websocket");
        return;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    s_connect_start_ms = muse_now_ms();
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        fail("could not start the websocket");
    }
    ESP_LOGI(TAG, "connecting to Deepgram");
}

static void close_ws(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        ESP_LOGI(TAG, "disconnected");
    }
    s_connected = s_ready = s_settings_sent = false;
    s_want_connect = false;
    s_turn_active = false;
    s_silence_ms = 0;
    wifi_low_latency(false);
}

// ---------------------------------------------------------------- task
// Open, set up and tear down the connection. Returns true once it is ready
// for audio.
static bool manage_connection(int64_t now)
{
    if (s_want_connect && !s_ws) {
        open_ws();
        return false;
    }
    if (!s_ws) {
        return false;
    }
    if (s_closed && !s_connected) {
        if (s_turn_active || g_muse.agent_state == MUSE_AGENT_THINKING) {
            fail("Deepgram closed the connection");
        } else {
            close_ws();
        }
        return false;
    }
    if (s_connected && !s_settings_sent) {
        send_settings();
        s_settings_sent = true;
    }
    if (!s_ready && now - s_connect_start_ms > CONNECT_TIMEOUT_MS) {
        fail("Deepgram did not answer in time");
    }
    return s_ready;
}

static void send_pending(int64_t now)
{
    static uint8_t chunk[SEND_CHUNK];
    static const uint8_t silence[640];
    // Microphone up, then a little silence after release.
    size_t n;
    while ((n = xStreamBufferReceive(s_mic, chunk, sizeof(chunk), 0)) > 0) {
        send_audio(chunk, n);
    }
    if (!s_turn_active && s_silence_ms > 0) {
        send_audio(silence, sizeof(silence));
        s_silence_ms -= 20;
    }
    if (s_fn_pending) {
        s_fn_pending = false;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "type", "FunctionCallResponse");
        cJSON_AddStringToObject(r, "id", s_fn_id);
        cJSON_AddStringToObject(r, "name", s_fn_name);
        cJSON_AddStringToObject(r, "content", "{\"ok\":true}");
        send_json(r);
    }
    if (!s_turn_active && s_silence_ms <= 0 && now - s_last_send_ms > KEEPALIVE_MS) {
        send_text("{\"type\":\"KeepAlive\"}");
    }
}

static void agent_task(void *arg)
{
    (void)arg;
    int64_t speech_empty_since = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        const int64_t now = muse_now_ms();

        if (s_failed) {
            s_failed = false;
            close_ws();
            set_state(agent_configured() ? MUSE_AGENT_IDLE : MUSE_AGENT_OFF);
            continue;
        }
        if (manage_connection(now)) {
            send_pending(now);
        }

        // The reply is finished once Deepgram is done and playback has drained.
        const int state = g_muse.agent_state;
        if (state == MUSE_AGENT_SPEAKING) {
            s_last_activity_ms = now;
            if ((s_audio_done || !s_ws) && xStreamBufferIsEmpty(s_speech)) {
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
            ESP_LOGW(TAG, "no reply");
            set_state(MUSE_AGENT_IDLE);
            wifi_low_latency(false);
        } else if (state == MUSE_AGENT_IDLE && s_ws && now - s_last_activity_ms > IDLE_CLOSE_MS) {
            close_ws();
        }
    }
}

// ---------------------------------------------------------------- public API
void agent_init(void)
{
    uint8_t *mic = heap_caps_malloc(MIC_BUFFER_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *speech = heap_caps_malloc(SPEECH_BUFFER_BYTES, MALLOC_CAP_SPIRAM);
    s_text = heap_caps_malloc(TEXT_MAX + 1, MALLOC_CAP_SPIRAM);
    s_send_lock = xSemaphoreCreateMutex();
    if (!mic || !speech || !s_text || !s_send_lock) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    s_mic = xStreamBufferCreateStatic(MIC_BUFFER_BYTES, 1, mic, &s_mic_ctrl);
    s_speech = xStreamBufferCreateStatic(SPEECH_BUFFER_BYTES, 1, speech, &s_speech_ctrl);
    g_muse.agent_action = -1;
    set_state(agent_configured() ? MUSE_AGENT_IDLE : MUSE_AGENT_OFF);
    xTaskCreatePinnedToCore(agent_task, "muse_agent", 6 * 1024, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "voice chat %s", agent_configured() ? "ready: hold the screen to talk" : "off: no Deepgram key yet");
}

void agent_refresh(void)
{
    const int state = g_muse.agent_state;
    if (state == MUSE_AGENT_OFF || state == MUSE_AGENT_IDLE) {
        set_state(agent_configured() ? MUSE_AGENT_IDLE : MUSE_AGENT_OFF);
    }
}

bool agent_configured(void)
{
    return settings_has(SETTING_DEEPGRAM_KEY);
}

bool agent_begin_turn(void)
{
    if (!s_mic) {
        return false;
    }
    if (!agent_configured()) {
        fail("no Deepgram key: hold BOOT for 3 s to open the setup page");
        return false;
    }
    if (!wifi_connected()) {
        fail("no WiFi");
        return false;
    }
    s_discard = true;  // stop any reply that is still playing
    s_silence_ms = 0;
    s_turn_active = true;
    s_last_activity_ms = muse_now_ms();
    wifi_low_latency(true);
    s_want_connect = true;
    set_state(s_ready ? MUSE_AGENT_LISTENING : MUSE_AGENT_CONNECTING);
    return true;
}

void agent_end_turn(void)
{
    if (!s_turn_active) {
        return;
    }
    s_turn_active = false;
    s_silence_ms = TAIL_SILENCE_MS;
    s_turn_end_ms = muse_now_ms();
    s_last_activity_ms = s_turn_end_ms;
    s_audio_done = false;
    s_discard = false;
    set_state(MUSE_AGENT_THINKING);
}

void agent_stop_speaking(void)
{
    s_discard = true;
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
    if (s_turn_active && s_ready && g_muse.agent_state == MUSE_AGENT_CONNECTING) {
        set_state(MUSE_AGENT_LISTENING);
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
