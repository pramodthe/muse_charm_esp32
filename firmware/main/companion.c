// SPDX-License-Identifier: MIT
// Claude Code companion client. See companion.h and companion/README.md.
//
// Where the server is: Muse first listens for a companion server on the local
// network (its UDP beacon). If there is none, it connects to the online server
// it was told about, over TLS, presenting its token. The address and token are
// saved in NVS when a local server sends them ("muse_companion.py pair"), or
// set at build time in menuconfig.
//
// Messages from the server (JSON text frames):
//   {"type":"hello","name":"pramods mac"}
//   {"type":"claude","mood":"working","caption":"Editing face.c"}
//   {"type":"claude","mood":"idle","caption":"","event":"done","event_caption":"Done!"}
//   {"type":"voice_ready","on":true}, {"type":"voice",...} and binary reply
//   audio, which go to agent.c
//   {"type":"server","url":"wss://...","token":"..."}  remember the online
//   server (an empty url forgets it); Muse answers {"type":"paired","url":...}
//   {"type":"alert","text":"Tea"}  a timer is up and Muse cannot say so
//
// Messages to the server: "hello", "status" (how Muse is doing, once a minute),
// "awake" (picked up, woken or switched on), and agent.c's voice messages.

#include "companion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent.h"
#include "audio.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_wifi.h"
#include "muse.h"
#include "nvs.h"
#include "panel.h"
#include "power.h"
#include "wifi.h"

static const char *TAG = "companion";

#define NVS_NAMESPACE "muse_link"
#define URL_MAX 160
#define TOKEN_MAX 96

#define BEACON_PORT 47800
#define BEACON_MAGIC "MUSE-COMPANION 1 "
#define LISTEN_MS 3000      // wait this long for a beacon per try
#define LOST_AFTER_MS 8000  // offline this long: look for the server again
#define LOST_AFTER_ONLINE_MS 20000  // the TLS handshake over the internet takes a few seconds
#define RX_MAX 1024
#define STATUS_EVERY_MS 60000
#define JUST_SWITCHED_ON_MS 90000  // linked this soon after power-up counts as being woken

static esp_websocket_client_handle_t s_ws;
static volatile bool s_connected, s_hello_due;
static volatile int64_t s_offline_since_ms;
static QueueHandle_t s_events;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_ws_lock;  // s_ws may be destroyed while another task sends
static int s_last_op;
static char s_mood_caption[COMPANION_CAPTION_LEN];
static char s_uri[URL_MAX];
static char s_headers[TOKEN_MAX + 32];
static bool s_using_online;                // s_uri is the online server
static char s_online_url[URL_MAX];         // "" if Muse knows of none
static char s_online_token[TOKEN_MAX];
static char s_new_url[URL_MAX], s_new_token[TOKEN_MAX];  // from a "server" message, to be saved
static volatile bool s_pair_due;
static volatile bool s_status_due;
static volatile bool s_awake_due;
static int64_t s_status_sent_ms;
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
    } else if (type && strcmp(type, "alert") == 0) {
        post_event(COMPANION_ALERT, cJSON_GetStringValue(cJSON_GetObjectItem(root, "text")));
    } else if (type && strcmp(type, "server") == 0) {
        // Remember (or forget) the online server; the companion task saves it.
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
        const char *token = cJSON_GetStringValue(cJSON_GetObjectItem(root, "token"));
        if (url && strlen(url) < URL_MAX && (!token || strlen(token) < TOKEN_MAX) && !s_using_online) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            snprintf(s_new_url, sizeof(s_new_url), "%s", url);
            snprintf(s_new_token, sizeof(s_new_token), "%s", token ? token : "");
            xSemaphoreGive(s_lock);
            s_pair_due = true;
        }
    } else if (type && strcmp(type, "voice_ready") == 0) {
        agent_on_voice_ready(cJSON_IsTrue(cJSON_GetObjectItem(root, "on")));
    } else if (type && strcmp(type, "voice") == 0) {
        agent_on_voice(root);
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
        s_status_due = true;
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

// ---------------------------------------------------------------- how Muse is doing
// {"type":"status","battery":87,"charging":true,"usb":true,"wifi":"Home","rssi":-58,
//  "pose":"upright","noise":34,"brightness":80,"volume":65,"uptime":754,"ram":41,"psram":5210}
// ("ram" and "psram": free memory in KB, internal and external)
static void send_status(void)
{
    char msg[384];
    int n = snprintf(msg, sizeof(msg), "{\"type\":\"status\"");
    muse_power_t power;
    if (power_read(&power)) {
        if (power.battery) {
            n += snprintf(msg + n, sizeof(msg) - n, ",\"battery\":%d,\"charging\":%s", power.percent,
                          power.charging ? "true" : "false");
        }
        n += snprintf(msg + n, sizeof(msg) - n, ",\"usb\":%s", power.usb ? "true" : "false");
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        char ssid[33];
        int k = 0;
        for (const uint8_t *c = ap.ssid; *c && k < 32; c++) {
            ssid[k++] = (*c >= 32 && *c < 127 && *c != '"' && *c != '\\') ? (char)*c : '?';
        }
        ssid[k] = '\0';
        n += snprintf(msg + n, sizeof(msg) - n, ",\"wifi\":\"%s\",\"rssi\":%d", ssid, ap.rssi);
    }
    const float tilt = g_muse.tilt_deg;
    const char *pose = g_muse.flat ? "lying flat" : (tilt > 25 || tilt < -25) ? "tipped on its side" : "upright";
    n += snprintf(msg + n, sizeof(msg) - n,
                  ",\"pose\":\"%s\",\"noise\":%d,\"brightness\":%d,\"volume\":%d,\"uptime\":%d"
                  ",\"ram\":%d,\"psram\":%d}", pose,
                  g_muse.noise_db, panel_awake_brightness(), audio_volume(), (int)(muse_now_ms() / 1000),
                  (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    if (n > 0 && n < (int)sizeof(msg)) {
        companion_send_text(msg);
    }
}

// ---------------------------------------------------------------- the online server
static void online_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_online_url);
        if (nvs_get_str(h, "url", s_online_url, &n) != ESP_OK) {
            s_online_url[0] = '\0';
        }
        n = sizeof(s_online_token);
        if (nvs_get_str(h, "token", s_online_token, &n) != ESP_OK) {
            s_online_token[0] = '\0';
        }
        nvs_close(h);
    }
    if (!s_online_url[0] && CONFIG_MUSE_COMPANION_URL[0]) {  // the build's default
        snprintf(s_online_url, sizeof(s_online_url), "%s", CONFIG_MUSE_COMPANION_URL);
        snprintf(s_online_token, sizeof(s_online_token), "%s", CONFIG_MUSE_COMPANION_TOKEN);
    }
    if (s_online_url[0]) {
        ESP_LOGI(TAG, "online server: %s", s_online_url);
    }
}

// Save what a "server" message brought, and tell the server.
static void online_save(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_online_url, sizeof(s_online_url), "%s", s_new_url);
    snprintf(s_online_token, sizeof(s_online_token), "%s", s_new_token);
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    bool saved = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        saved = nvs_set_str(h, "url", s_online_url) == ESP_OK && nvs_set_str(h, "token", s_online_token) == ESP_OK &&
                nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    if (!saved) {
        ESP_LOGW(TAG, "could not save the online server");
        return;
    }
    char reply[URL_MAX + 40];
    if (s_online_url[0]) {
        ESP_LOGI(TAG, "saved the online server: %s", s_online_url);
        snprintf(reply, sizeof(reply), "{\"type\":\"paired\",\"url\":\"%s\"}", s_online_url);
    } else {
        ESP_LOGI(TAG, "forgot the online server");
        snprintf(reply, sizeof(reply), "{\"type\":\"paired\",\"url\":\"\"}");
    }
    companion_send_text(reply);
}

// ---------------------------------------------------------------- finding the server
// Listen for a local server's beacon ("MUSE-COMPANION 1 <port> <name>") and
// build the websocket address from where it came from. Without one, fall back
// to the online server, if Muse knows of one.
static bool discover(char *uri, size_t len)
{
    s_using_online = false;
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
    if (!found && s_online_url[0]) {
        snprintf(uri, len, "%s", s_online_url);
        s_using_online = true;
        found = true;
    }
    return found;
}

static void open_ws(void)
{
    esp_websocket_client_config_t cfg = {
        .uri = s_uri,
        .buffer_size = 2048,
        .task_stack = 4096,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 3000,
        .ping_interval_sec = 10,
    };
    if (strncmp(s_uri, "wss://", 6) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;  // check the server's certificate
        cfg.task_stack = 8192;                          // room for the TLS handshake
        cfg.network_timeout_ms = 10000;
    }
    if (s_using_online && s_online_token[0]) {
        snprintf(s_headers, sizeof(s_headers), "Authorization: Bearer %s\r\n", s_online_token);
        cfg.headers = s_headers;
    }
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
                if (muse_now_ms() < JUST_SWITCHED_ON_MS) {
                    s_awake_due = true;  // switched on: someone is here
                }
            }
            if (s_connected && (s_status_due || muse_now_ms() - s_status_sent_ms > STATUS_EVERY_MS)) {
                s_status_due = false;
                s_status_sent_ms = muse_now_ms();
                send_status();
            }
            if (s_awake_due && s_connected) {
                s_awake_due = false;
                companion_send_text("{\"type\":\"awake\"}");
            }
            if (s_pair_due && s_connected) {
                s_pair_due = false;
                online_save();
            }
            if (!s_connected &&
                    muse_now_ms() - s_offline_since_ms > (s_using_online ? LOST_AFTER_ONLINE_MS : LOST_AFTER_MS)) {
                ESP_LOGI(TAG, "no answer from %s; looking for the server again", s_uri);
                destroy_ws();
                said_looking = false;
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        if (!said_looking) {
            ESP_LOGI(TAG, "looking for a companion server on this network (UDP port %d)%s", BEACON_PORT,
                     s_online_url[0] ? ", then online" : "");
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
    online_load();
    g_muse.claude_mood = MUSE_MOOD_OFF;
    xTaskCreatePinnedToCore(companion_task, "muse_companion", 6144, NULL, 2, NULL, 0);
}

bool companion_connected(void)
{
    return s_connected;
}

void companion_report_status(void)
{
    s_status_due = true;
}

void companion_report_awake(void)
{
    if (s_connected) {  // only as it happens: no late good mornings after a reconnect
        s_status_due = true;
        s_awake_due = true;
    }
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
