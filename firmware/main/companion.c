// SPDX-License-Identifier: MIT
// Claude Code companion client. See companion.h and companion/README.md.
//
// Messages from the server (JSON text frames):
//   {"type":"hello","name":"pramods mac"}
//   {"type":"claude","mood":"working","caption":"Editing face.c"}
//   {"type":"claude","mood":"idle","caption":"","event":"done","event_caption":"Done!"}
//   {"type":"voice_ready","on":true}, {"type":"voice",...} and binary reply
//   audio, which go to agent.c

#include "companion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "muse.h"
#include "wifi.h"

static const char *TAG = "companion";

#define BEACON_PORT 47800
#define BEACON_MAGIC "MUSE-COMPANION 1 "
#define LISTEN_MS 3000      // wait this long for a beacon per try
#define LOST_AFTER_MS 8000  // offline this long: look for the server again
#define RX_MAX 1024

static esp_websocket_client_handle_t s_ws;
static volatile bool s_connected, s_hello_due;
static volatile int64_t s_offline_since_ms;
static QueueHandle_t s_events;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_ws_lock;  // s_ws may be destroyed while another task sends
static int s_last_op;
static char s_mood_caption[COMPANION_CAPTION_LEN];
static char s_uri[96];
static char s_rx[RX_MAX + 1];  // one message, reassembled if it came in pieces

// ---------------------------------------------------------------- messages
static int mood_from(const char *s)
{
    if (s && strcmp(s, "thinking") == 0) {
        return MUSE_MOOD_THINKING;
    }
    if (s && strcmp(s, "working") == 0) {
        return MUSE_MOOD_WORKING;
    }
    if (s && strcmp(s, "needs_you") == 0) {
        return MUSE_MOOD_NEEDS_YOU;
    }
    return MUSE_MOOD_IDLE;
}

static int event_from(const char *s)
{
    static const char *names[] = {"linked", "hello", "done", "oops", "error", "compact", "bye"};
    for (int i = 0; s && i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        if (strcmp(s, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static void post_event(int kind, const char *caption)
{
    companion_event_t e = {.kind = (companion_event_kind_t)kind};
    snprintf(e.caption, sizeof(e.caption), "%s", caption ? caption : "");
    if (xQueueSend(s_events, &e, 0) != pdTRUE) {
        ESP_LOGW(TAG, "too many events; dropped one");
    }
}

static void handle_message(const char *text, int len)
{
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) {
        return;
    }
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (type && strcmp(type, "hello") == 0) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
        char caption[COMPANION_CAPTION_LEN];
        snprintf(caption, sizeof(caption), "Linked to %.30s", name && name[0] ? name : "your computer");
        ESP_LOGI(TAG, "%s", caption);
        post_event(COMPANION_LINKED, caption);
    } else if (type && strcmp(type, "voice_ready") == 0) {
        agent_on_voice_ready(cJSON_IsTrue(cJSON_GetObjectItem(root, "on")));
    } else if (type && strcmp(type, "voice") == 0) {
        const cJSON *cps = cJSON_GetObjectItem(root, "cps");
        agent_on_voice(cJSON_GetStringValue(cJSON_GetObjectItem(root, "state")),
                       cJSON_GetStringValue(cJSON_GetObjectItem(root, "action")),
                       cJSON_GetStringValue(cJSON_GetObjectItem(root, "error")),
                       cJSON_GetStringValue(cJSON_GetObjectItem(root, "heard")),
                       cJSON_GetStringValue(cJSON_GetObjectItem(root, "say")),
                       cJSON_IsNumber(cps) ? cps->valueint : 0);
    } else if (type && strcmp(type, "claude") == 0) {
        const char *mood = cJSON_GetStringValue(cJSON_GetObjectItem(root, "mood"));
        const char *caption = cJSON_GetStringValue(cJSON_GetObjectItem(root, "caption"));
        const char *event = cJSON_GetStringValue(cJSON_GetObjectItem(root, "event"));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_mood_caption, sizeof(s_mood_caption), "%s", caption ? caption : "");
        xSemaphoreGive(s_lock);
        const int kind = event_from(event);
        if (kind >= 0) {
            post_event(kind, cJSON_GetStringValue(cJSON_GetObjectItem(root, "event_caption")));
        }
        g_muse.claude_mood = mood_from(mood);
        ESP_LOGI(TAG, "%s%s%s  \"%s\"", mood ? mood : "?", event ? " + " : "", event ? event : "",
                 caption ? caption : "");
    }
    cJSON_Delete(root);
}

static void go_offline(void)
{
    if (s_connected || g_muse.claude_mood != MUSE_MOOD_OFF) {
        ESP_LOGI(TAG, "offline");
    }
    s_connected = false;
    s_offline_since_ms = muse_now_ms();
    g_muse.claude_mood = MUSE_MOOD_OFF;
    agent_on_link_lost();
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to %s", s_uri);
        s_connected = true;
        s_hello_due = true;
        g_muse.claude_mood = MUSE_MOOD_IDLE;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_connected) {
            go_offline();
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1 || d->op_code == 0x2) {
            s_last_op = d->op_code;
        } else if (d->op_code != 0x0) {
            break;  // pings, pongs and close frames
        }
        if (s_last_op == 0x2) {
            agent_on_audio(d->data_ptr, d->data_len);  // reply audio
            break;
        }
        if (d->payload_len > RX_MAX || d->payload_offset + d->data_len > RX_MAX) {
            break;  // not from our server
        }
        memcpy(s_rx + d->payload_offset, d->data_ptr, d->data_len);
        if (d->payload_offset + d->data_len >= d->payload_len) {
            handle_message(s_rx, d->payload_len);
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- finding the server
// Listen for the server's beacon ("MUSE-COMPANION 1 <port> <name>") and build
// the websocket address from where it came from.
static bool discover(char *uri, size_t len)
{
    if (CONFIG_MUSE_COMPANION_URL[0]) {
        snprintf(uri, len, "%s", CONFIG_MUSE_COMPANION_URL);
        vTaskDelay(pdMS_TO_TICKS(LISTEN_MS));  // pace the retries
        return true;
    }
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        vTaskDelay(pdMS_TO_TICKS(LISTEN_MS));
        return false;
    }
    const int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    const struct timeval tv = {.tv_sec = LISTEN_MS / 1000, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(BEACON_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bool found = false;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char buf[96];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len);
        if (n > (int)strlen(BEACON_MAGIC)) {
            buf[n] = '\0';
            const int port = atoi(buf + strlen(BEACON_MAGIC));
            if (strncmp(buf, BEACON_MAGIC, strlen(BEACON_MAGIC)) == 0 && port > 0 && port < 65536) {
                char ip[16];
                inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
                snprintf(uri, len, "ws://%s:%d/device", ip, port);
                found = true;
            }
        }
    } else {
        vTaskDelay(pdMS_TO_TICKS(LISTEN_MS));
    }
    close(sock);
    return found;
}

static void open_ws(void)
{
    const esp_websocket_client_config_t cfg = {
        .uri = s_uri,
        .buffer_size = 2048,
        .task_stack = 4096,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 3000,
        .ping_interval_sec = 10,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "could not create the websocket");
        return;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws_event, NULL);
    s_offline_since_ms = muse_now_ms();
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        ESP_LOGE(TAG, "could not start the websocket");
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
}

static void destroy_ws(void)
{
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    xSemaphoreGive(s_ws_lock);
}

static void companion_task(void *arg)
{
    (void)arg;
    bool said_looking = false;
    for (;;) {
        if (!wifi_connected()) {
            if (s_ws) {
                destroy_ws();
                go_offline();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_ws) {
            if (s_hello_due && s_connected) {
                s_hello_due = false;
                companion_send_text("{\"type\":\"hello\",\"name\":\"Muse\",\"voice\":1}");
            }
            if (!s_connected && muse_now_ms() - s_offline_since_ms > LOST_AFTER_MS) {
                ESP_LOGI(TAG, "no answer from %s; looking for the server again", s_uri);
                destroy_ws();
                said_looking = false;
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        if (!said_looking) {
            ESP_LOGI(TAG, "looking for the companion server on UDP port %d", BEACON_PORT);
            said_looking = true;
        }
        if (discover(s_uri, sizeof(s_uri))) {
            ESP_LOGI(TAG, "found the companion server at %s", s_uri);
            open_ws();
        }
    }
}

// ---------------------------------------------------------------- API
void companion_start(void)
{
    s_events = xQueueCreate(6, sizeof(companion_event_t));
    s_lock = xSemaphoreCreateMutex();
    s_ws_lock = xSemaphoreCreateMutex();
    g_muse.claude_mood = MUSE_MOOD_OFF;
    xTaskCreatePinnedToCore(companion_task, "muse_companion", 4096, NULL, 2, NULL, 0);
}

bool companion_connected(void)
{
    return s_connected;
}

bool companion_next_event(companion_event_t *out)
{
    return s_events && xQueueReceive(s_events, out, 0) == pdTRUE;
}

bool companion_event_waiting(void)
{
    return s_events && uxQueueMessagesWaiting(s_events) > 0;
}

static bool ws_send(bool text, const void *data, size_t len)
{
    if (!s_ws_lock || !s_connected) {
        return false;
    }
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    int n = -1;
    if (s_ws && s_connected) {
        // A send that times out makes the client drop the connection, so allow
        // for a slow WiFi moment rather than giving up after a fraction of a second.
        n = text ? esp_websocket_client_send_text(s_ws, data, len, pdMS_TO_TICKS(5000))
                 : esp_websocket_client_send_bin(s_ws, data, len, pdMS_TO_TICKS(5000));
    }
    xSemaphoreGive(s_ws_lock);
    return n >= 0;
}

bool companion_send_text(const char *text)
{
    return ws_send(true, text, strlen(text));
}

bool companion_send_binary(const void *data, size_t len)
{
    return ws_send(false, data, len);
}

void companion_mood_caption(char *buf, size_t len)
{
    if (!s_lock) {
        buf[0] = '\0';
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(buf, len, "%s", s_mood_caption);
    xSemaphoreGive(s_lock);
}
