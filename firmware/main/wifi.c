// SPDX-License-Identifier: MIT
// WiFi: saved networks, auto-reconnect and the "Muse-setup" portal.
// Adapted from net_prov in esp32-agui (multi-SSID connect, backoff reconnect,
// low-latency power-save switching).

#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "muse.h"
#include "nvs.h"
#include "portal.h"

static const char *TAG = "wifi";

#define NVS_NAMESPACE "netprov"
#define MAX_NETWORKS 5
#define CONNECT_TIMEOUT_MS 12000
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000
#define SETUP_TIMEOUT_MS (10 * 60 * 1000)
#define WIFI_MAX_TX_POWER 56  // 0.25 dBm units: 14 dBm

#define BIT_CONNECTED BIT0
#define BIT_FAILED BIT1
#define BIT_SETUP BIT2

typedef struct {
    char ssid[33];
    char pass[65];
} network_t;

static EventGroupHandle_t s_events;
static esp_timer_handle_t s_reconnect_timer;
static volatile bool s_connecting, s_auto_reconnect, s_online, s_setup_active, s_hold;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static volatile int s_last_reason;

// ---------------------------------------------------------------- saved networks
static int creds_load(network_t *out, int max)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    uint8_t count = 0;
    nvs_get_u8(h, "count", &count);
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        char ks[12], kp[12];
        snprintf(ks, sizeof(ks), "ssid%d", i);
        snprintf(kp, sizeof(kp), "pass%d", i);
        size_t ls = sizeof(out[n].ssid), lp = sizeof(out[n].pass);
        out[n].pass[0] = '\0';
        if (nvs_get_str(h, ks, out[n].ssid, &ls) != ESP_OK) {
            continue;
        }
        nvs_get_str(h, kp, out[n].pass, &lp);
        n++;
    }
    nvs_close(h);
    return n;
}

esp_err_t wifi_creds_add(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t count = 0;
    nvs_get_u8(h, "count", &count);
    // Replace an existing entry for the same network, else append (overwriting the last when full).
    int slot = -1;
    for (int i = 0; i < count; i++) {
        char ks[12], existing[33];
        size_t len = sizeof(existing);
        snprintf(ks, sizeof(ks), "ssid%d", i);
        if (nvs_get_str(h, ks, existing, &len) == ESP_OK && strcmp(existing, ssid) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = count < MAX_NETWORKS ? count : MAX_NETWORKS - 1;
        count = slot + 1;
    }
    char ks[12], kp[12];
    snprintf(ks, sizeof(ks), "ssid%d", slot);
    snprintf(kp, sizeof(kp), "pass%d", slot);
    err = nvs_set_str(h, ks, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, kp, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "count", count);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "saved network %s (%s)", ssid, esp_err_to_name(err));
    return err;
}

bool wifi_has_credentials(void)
{
    network_t n;
    return creds_load(&n, 1) > 0;
}

// ---------------------------------------------------------------- events
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }
    s_online = false;
    const wifi_event_sta_disconnected_t *d = data;
    if (s_connecting) {
        s_last_reason = d ? d->reason : -1;
        ESP_LOGW(TAG, "connect failed (reason %d)", s_last_reason);
        xEventGroupSetBits(s_events, BIT_FAILED);
    } else if (s_auto_reconnect) {
        ESP_LOGW(TAG, "link dropped (reason %d), retrying in %u ms", d ? d->reason : -1, (unsigned)s_backoff_ms);
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)s_backoff_ms * 1000);
        s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "online: " IPSTR, IP2STR(&e->ip_info.ip));
        s_online = true;
        s_backoff_ms = BACKOFF_MIN_MS;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

static void reconnect_cb(void *arg)
{
    if (s_auto_reconnect && !s_online) {
        esp_wifi_connect();
    }
}

// Accept any personal security mode, including WPA3 (SAE) networks.
static void station_config(wifi_config_t *wc, const char *ssid, const char *pass)
{
    memset(wc, 0, sizeof(*wc));
    strlcpy((char *)wc->sta.ssid, ssid, sizeof(wc->sta.ssid));
    strlcpy((char *)wc->sta.password, pass ? pass : "", sizeof(wc->sta.password));
    wc->sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    wc->sta.pmf_cfg.capable = true;
    wc->sta.pmf_cfg.required = false;
    wc->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
}

// Try each saved network in turn, giving up early on a failure. A network that
// is in range but fails (a weak signal can time out the handshake) gets a
// second try. *in_range tells whether any saved network was there at all.
static bool connect_saved(bool *in_range)
{
    network_t nets[MAX_NETWORKS];
    const int n = creds_load(nets, MAX_NETWORKS);
    *in_range = false;
    for (int i = 0; i < n; i++) {
        for (int attempt = 0; attempt < 2; attempt++) {
            wifi_config_t wc;
            station_config(&wc, nets[i].ssid, nets[i].pass);
            esp_wifi_set_config(WIFI_IF_STA, &wc);
            ESP_LOGI(TAG, "trying %s", nets[i].ssid);
            xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
            s_last_reason = 0;
            s_connecting = true;
            esp_wifi_connect();
            const EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED, pdTRUE, pdFALSE,
                                                         pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
            s_connecting = false;
            if (bits & BIT_CONNECTED) {
                *in_range = true;
                return true;
            }
            esp_wifi_disconnect();
            if (s_last_reason == WIFI_REASON_NO_AP_FOUND) {
                break;  // not here
            }
            *in_range = true;
        }
    }
    return false;
}

static void run_setup(void)
{
    // Background reconnects scan every channel, which drops phones off the
    // setup network, so pause them while it's open.
    s_auto_reconnect = false;
    esp_timer_stop(s_reconnect_timer);
    s_setup_active = true;
    g_muse.setup_active = true;
    if (portal_start(WIFI_SETUP_SSID) == ESP_OK) {
        portal_wait_saved(SETUP_TIMEOUT_MS);
        vTaskDelay(pdMS_TO_TICKS(2000));  // let the phone receive the "saved" page
        portal_stop();
    }
    s_setup_active = false;
    g_muse.setup_active = false;
    s_auto_reconnect = true;
}

static void wifi_task(void *arg)
{
    (void)arg;
    bool offered_setup = false;
    if (!wifi_has_credentials()) {
        ESP_LOGI(TAG, "no saved WiFi: opening setup on the screen");
        offered_setup = true;
        g_muse.setup_request = true;
    }
    for (;;) {
        if (!s_online && !s_hold && wifi_has_credentials()) {
            s_auto_reconnect = false;
            bool in_range;
            const bool connected = connect_saved(&in_range);
            s_auto_reconnect = !s_hold;
            if (!connected && !in_range && !offered_setup) {
                // New place: offer setup once per start-up.
                ESP_LOGI(TAG, "no saved network in range: opening setup on the screen");
                offered_setup = true;
                g_muse.setup_request = true;
            }
            if (!connected) {
                ESP_LOGW(TAG, "no saved network reachable, retrying in the background");
            }
        }
        // Wait for a setup request, re-checking the link every 30 s.
        const EventBits_t bits = xEventGroupWaitBits(s_events, BIT_SETUP, pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
        if (bits & BIT_SETUP) {
            run_setup();
        }
    }
}

// ---------------------------------------------------------------- public API
void wifi_start(void)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    const esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(e);
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    // Full transmit power (20 dBm) makes current spikes that dip the board's
    // supply and can blank the AMOLED, notably when the setup network starts.
    esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER);

    const esp_timer_create_args_t targs = {.callback = reconnect_cb, .name = "wifi_reconnect"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));
    xTaskCreatePinnedToCore(wifi_task, "muse_wifi", 4096, NULL, 3, NULL, 0);
}

bool wifi_connected(void)
{
    return s_online;
}

void wifi_open_setup(void)
{
    if (s_events && !s_setup_active) {
        xEventGroupSetBits(s_events, BIT_SETUP);
    }
}

bool wifi_setup_active(void)
{
    return s_setup_active;
}

void wifi_hold(bool hold)
{
    s_hold = hold;
    s_auto_reconnect = !hold;
    if (hold) {
        esp_timer_stop(s_reconnect_timer);
    }
}

int wifi_scan(wifi_ap_record_t *out, int max)
{
    for (int i = 0; i < 150 && s_connecting; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));  // let a background attempt finish
    }
    if (!s_online) {
        esp_wifi_disconnect();  // a pending connect blocks scanning
    }
    const wifi_scan_config_t cfg = {.show_hidden = false};
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed");
        return 0;
    }
    uint16_t n = max;
    if (esp_wifi_scan_get_ap_records(&n, out) != ESP_OK) {
        return 0;
    }
    ESP_LOGI(TAG, "scan found %u networks", n);
    return n;
}

bool wifi_try_connect(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    for (int i = 0; i < 150 && s_connecting; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_online) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    wifi_config_t wc;
    station_config(&wc, ssid, pass);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    ESP_LOGI(TAG, "joining %s", ssid);
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    s_connecting = true;
    esp_wifi_connect();
    const EventBits_t bits =
        xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    s_connecting = false;
    if (bits & BIT_CONNECTED) {
        wifi_creds_add(ssid, pass);
        return true;
    }
    esp_wifi_disconnect();
    return false;
}

bool wifi_current_ssid(char *buf, size_t len)
{
    wifi_ap_record_t ap;
    if (!s_online || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    strlcpy(buf, (const char *)ap.ssid, len);
    return true;
}

void wifi_low_latency(bool on)
{
    // Modem sleep adds ~100 ms per round trip, which throttles audio upload (see esp32-agui).
    esp_wifi_set_ps(on ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
}
