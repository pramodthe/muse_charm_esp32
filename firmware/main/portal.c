// SPDX-License-Identifier: MIT
// Setup portal: SoftAP + web form + DNS catch-all so phones open the page
// automatically. Adapted from net_prov/portal.c in esp32-agui.
//
// The form sets WiFi, the Deepgram API key, the voice and Muse's personality.
// Fields left empty keep their saved value; the key is never shown back.

#include "portal.h"

#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "portal";

#define PORTAL_IP "192.168.4.1"
#define BIT_SAVED BIT0
#define BODY_MAX 8192

static httpd_handle_t s_httpd;
static esp_netif_t *s_ap_netif;
static EventGroupHandle_t s_events;
static volatile bool s_dns_run;

static const char *const VOICES[] = {
    "aura-2-thalia-en", "aura-2-andromeda-en", "aura-2-helena-en", "aura-2-asteria-en", "aura-2-luna-en",
    "aura-2-apollo-en", "aura-2-arcas-en",     "aura-2-aries-en",  "aura-2-orion-en",   "aura-2-zeus-en",
};

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport "
    "content='width=device-width,initial-scale=1'><title>Muse setup</title>"
    "<style>body{font-family:system-ui,sans-serif;margin:1.5em;max-width:32em;color:#222}"
    "label{display:block;margin-top:1em;font-weight:600}"
    "input,textarea{display:block;width:100%%;box-sizing:border-box;padding:.6em;margin:.3em 0;font-size:1em}"
    "textarea{height:9em}small{color:#666}button{margin-top:1.2em;padding:.8em 1.4em;font-size:1em}"
    "</style></head><body><h2>Muse setup</h2><form method=POST action=/save>"
    "<label>WiFi network</label><input name=ssid autocomplete=off>"
    "<label>WiFi password</label><input name=pass type=password>"
    "<label>Deepgram API key</label><input name=key type=password autocomplete=off "
    "placeholder='%s'>"
    "<small>From console.deepgram.com. Leave empty to keep the saved key.</small>"
    "<label>Voice</label><input name=voice list=voices value='%s'><datalist id=voices>";

static const char PAGE_TAIL[] =
    "<small>Muse's personality. Empty uses the default.</small>"
    "<button type=submit>Save</button></form>"
    "<p><small>Leave WiFi empty to keep the current network. After saving, Muse reconnects "
    "and you can hold the screen to talk.</small></p></body></html>";

// ---------------------------------------------------------------- helpers
static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Find field `name` in an urlencoded body and decode it into out.
static bool form_field(const char *body, const char *name, char *out, size_t outsz)
{
    const size_t nl = strlen(name);
    for (const char *p = body; (p = strstr(p, name)) != NULL; p += nl) {
        if ((p != body && p[-1] != '&') || p[nl] != '=') {
            continue;
        }
        p += nl + 1;
        size_t o = 0;
        for (; *p && *p != '&' && o + 1 < outsz; p++) {
            if (*p == '+') {
                out[o++] = ' ';
            } else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
                out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
                p += 2;
            } else {
                out[o++] = *p;
            }
        }
        out[o] = '\0';
        return true;
    }
    return false;
}

// Append text to the response with HTML special characters escaped.
static void send_escaped(httpd_req_t *req, const char *s)
{
    char buf[256];
    size_t o = 0;
    for (; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '&': rep = "&amp;"; break;
        case '\'': rep = "&#39;"; break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        const size_t need = rep ? strlen(rep) : 1;
        if (o + need >= sizeof(buf)) {
            httpd_resp_send_chunk(req, buf, o);
            o = 0;
        }
        if (rep) {
            memcpy(buf + o, rep, need);
        } else {
            buf[o] = *s;
        }
        o += need;
    }
    if (o) {
        httpd_resp_send_chunk(req, buf, o);
    }
}

// ---------------------------------------------------------------- handlers
static esp_err_t root_get(httpd_req_t *req)
{
    static char value[SETTING_MAX_LEN];
    char head[sizeof(PAGE_HEAD) + 160];
    char voice[96];
    if (!settings_get(SETTING_VOICE, voice, sizeof(voice))) {
        strlcpy(voice, CONFIG_MUSE_AGENT_DEFAULT_VOICE, sizeof(voice));
    }
    const bool have_key = settings_has(SETTING_DEEPGRAM_KEY);
    snprintf(head, sizeof(head), PAGE_HEAD, have_key ? "saved (leave empty to keep)" : "paste your key", voice);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
    for (size_t i = 0; i < sizeof(VOICES) / sizeof(VOICES[0]); i++) {
        char opt[64];
        snprintf(opt, sizeof(opt), "<option value='%s'>", VOICES[i]);
        httpd_resp_send_chunk(req, opt, HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_send_chunk(req, "</datalist><label>Personality</label><textarea name=prompt placeholder='", HTTPD_RESP_USE_STRLEN);
    send_escaped(req, CONFIG_MUSE_AGENT_PROMPT);
    httpd_resp_send_chunk(req, "'>", HTTPD_RESP_USE_STRLEN);
    if (settings_get(SETTING_PERSONALITY, value, sizeof(value))) {
        send_escaped(req, value);
    }
    httpd_resp_send_chunk(req, "</textarea>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, PAGE_TAIL, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t save_post(httpd_req_t *req)
{
    char *body = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM);
    char *value = heap_caps_malloc(SETTING_MAX_LEN, MALLOC_CAP_SPIRAM);
    if (!body || !value) {
        free(body);
        free(value);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    const int total = req->content_len < BODY_MAX - 1 ? req->content_len : BODY_MAX - 1;
    int got = 0;
    while (got < total) {
        const int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            free(body);
            free(value);
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    char ssid[33] = {0}, pass[65] = {0};
    const bool wifi = form_field(body, "ssid", ssid, sizeof(ssid)) && ssid[0];
    if (wifi) {
        form_field(body, "pass", pass, sizeof(pass));
        wifi_creds_add(ssid, pass);
    }
    const bool key = form_field(body, "key", value, SETTING_MAX_LEN) && value[0];
    if (key) {
        settings_set(SETTING_DEEPGRAM_KEY, value);
    }
    if (form_field(body, "voice", value, SETTING_MAX_LEN)) {
        settings_set(SETTING_VOICE, value);  // empty falls back to the default voice
    }
    if (form_field(body, "prompt", value, SETTING_MAX_LEN)) {
        settings_set(SETTING_PERSONALITY, value);  // empty falls back to the default personality
    }
    ESP_LOGI(TAG, "saved: wifi %s, key %s", wifi ? "updated" : "unchanged", key ? "updated" : "unchanged");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    snprintf(body, BODY_MAX,
             "<!doctype html><html><head><meta charset=utf-8><meta name=viewport "
             "content='width=device-width,initial-scale=1'></head>"
             "<body style='font-family:system-ui,sans-serif;margin:1.5em'><h3>Saved</h3><ul>"
             "<li>WiFi: <b>%s</b></li><li>Deepgram key: <b>%s</b></li></ul>"
             "<p>Muse is reconnecting. Hold the screen to talk.</p></body></html>",
             wifi ? "updated" : "unchanged", key ? "updated" : "unchanged");
    httpd_resp_sendstr(req, body);
    free(body);
    free(value);
    xEventGroupSetBits(s_events, BIT_SAVED);
    return ESP_OK;
}

// Any other URL: redirect to the form (makes phones show the portal).
static esp_err_t redirect_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

// ---------------------------------------------------------------- DNS catch-all
// Answers every A query with the portal address.
static void dns_task(void *arg)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = {.tv_sec = 1};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        const int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &cl);
        if (n < 12 || n + 16 > (int)sizeof(buf)) {
            continue;
        }
        buf[2] = 0x81;  // response, recursion desired + available
        buf[3] = 0x80;
        buf[6] = 0x00;  // one answer
        buf[7] = 0x01;
        uint8_t *a = buf + n;
        const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
        memcpy(a, answer, sizeof(answer));
        sendto(sock, buf, n + sizeof(answer), 0, (struct sockaddr *)&cli, cl);
    }
    close(sock);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- public API
esp_err_t portal_start(const char *ap_ssid)
{
    if (!s_events) {
        s_events = xEventGroupCreate();
    }
    xEventGroupClearBits(s_events, BIT_SAVED);
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_set_max_tx_power(56);  // 14 dBm: the phone is close, and it keeps the supply steady

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        return ESP_FAIL;
    }
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    const httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = redirect_get};
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &any);

    s_dns_run = true;
    xTaskCreate(dns_task, "portal_dns", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "setup portal up: join WiFi '%s', then open http://%s/", ap_ssid, PORTAL_IP);
    return ESP_OK;
}

bool portal_wait_saved(uint32_t timeout_ms)
{
    const EventBits_t b = xEventGroupWaitBits(s_events, BIT_SAVED, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_SAVED) != 0;
}

void portal_stop(void)
{
    s_dns_run = false;
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(1100));  // let the DNS task leave its 1 s receive and exit
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup portal closed");
}
