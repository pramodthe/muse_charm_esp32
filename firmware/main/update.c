// SPDX-License-Identifier: MIT
// Firmware updates over WiFi. See update.h.

#include "update.h"

#include <stdio.h>
#include <string.h>

#include "companion.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "face.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "muse.h"

static const char *TAG = "update";

#define ID_LEN 65                           // a SHA-256 in hex
#define QUIET_MS 20000                      // Muse has been left alone this long before an update starts
#define CONFIRM_WITHIN_MS (10 * 60 * 1000)  // a new image must link to a server within this, or it is dropped
#define RETRY_AFTER_MS (5 * 60 * 1000)
#define TRIES 3                             // downloads tried for one offer

static char s_running[ID_LEN], s_version[33];
static char s_failed[ID_LEN];               // the image that was dropped after a trial, if any
static char s_offer[ID_LEN];
static int s_offer_size;
static SemaphoreHandle_t s_lock;
static volatile bool s_offer_due, s_busy, s_on_trial, s_linked, s_refused_due;
static char s_url[200], s_auth[128];
static int s_tries;
static int64_t s_quiet_since, s_next_try_ms;

static void log_memory(const char *when)
{
    ESP_LOGI(TAG, "%s: internal RAM free %u KB (largest block %u KB)", when,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

static void hex_id(const uint8_t *sha, char *out)
{
    for (int i = 0; i < 32; i++) {
        sprintf(out + 2 * i, "%02x", sha[i]);
    }
}

static bool partition_id(const esp_partition_t *part, char *out)
{
    uint8_t sha[32];
    out[0] = '\0';
    if (!part || esp_partition_get_sha256(part, sha) != ESP_OK) {
        return false;
    }
    hex_id(sha, out);
    return true;
}

static void report(const char *state, int percent, const char *why)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "{\"type\":\"update\",\"state\":\"%s\",\"percent\":%d,\"why\":\"%s\"}", state, percent,
             why ? why : "");
    companion_send_text(msg);
}

static esp_err_t add_token(esp_http_client_handle_t client)
{
    return esp_http_client_set_header(client, "Authorization", s_auth);
}

static bool fail(esp_https_ota_handle_t handle, const char *why)
{
    ESP_LOGW(TAG, "update failed: %s", why);
    if (handle) {
        esp_https_ota_abort(handle);
    }
    report("failed", 0, why);
    lvgl_port_resume();
    face_show_sign("Update failed", 4000);
    s_busy = false;
    return false;
}

// Download the image the server offered into the other app slot and make it
// the one to start. True when Muse should restart.
static bool install(const char *id, int size)
{
    if (!companion_update_source(s_url, sizeof(s_url), s_auth, sizeof(s_auth))) {
        ESP_LOGW(TAG, "updates come from the online server only (wss://, with a token)");
        return false;
    }
    s_busy = true;
    ESP_LOGI(TAG, "downloading %.8s (%d KB)", id, size / 1024);
    report("downloading", 0, NULL);
    // The download needs most of the internal memory that is left, and the
    // display needs a block of it for every stripe it draws. So the screen
    // holds still, with a sign up, until Muse restarts.
    face_show_sign("Updating...", 30 * 60 * 1000);
    vTaskDelay(pdMS_TO_TICKS(600));  // let the sign be drawn
    lvgl_port_stop();
    log_memory("before the download");

    const esp_http_client_config_t http = {
        .url = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,  // check the server's certificate
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
    };
    const esp_https_ota_config_t config = {
        .http_config = &http,
        .http_client_init_cb = add_token,
        .buffer_caps = MALLOC_CAP_SPIRAM,  // internal memory is scarce
    };
    esp_https_ota_handle_t handle = NULL;
    if (esp_https_ota_begin(&config, &handle) != ESP_OK) {
        return fail(NULL, "could not start the download");
    }
    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(handle, &desc) != ESP_OK ||
            strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)) != 0) {
        return fail(handle, "that is not Muse firmware");
    }
    int shown = -1, told = 0;
    esp_err_t err;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int total = esp_https_ota_get_image_size(handle);
        if (total <= 0) {
            total = size > 0 ? size : 1;
        }
        const int percent = (int)((int64_t)esp_https_ota_get_image_len_read(handle) * 100 / total);
        if (percent != shown) {
            shown = percent;
            if (percent >= told + 10) {
                told = percent - percent % 10;
                ESP_LOGI(TAG, "%d %%", told);
                report("downloading", told, NULL);
                if (told == 10) {
                    log_memory("while downloading");
                }
            }
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        return fail(handle, "the download stopped");
    }
    if (esp_https_ota_finish(handle) != ESP_OK) {  // checks the image, then makes it the one to start
        return fail(NULL, "the downloaded image is damaged");
    }
    // The server said which image this would be: make sure it is that one.
    char got[ID_LEN];
    if (!partition_id(esp_ota_get_boot_partition(), got) || strcmp(got, id) != 0) {
        esp_ota_set_boot_partition(esp_ota_get_running_partition());
        return fail(NULL, "not the image the server offered");
    }
    ESP_LOGI(TAG, "installed %.8s (%s); restarting", id, desc.version);
    report("restarting", 100, NULL);
    return true;
}

// Its own task, only while an update is installed: the TLS handshake needs the
// stack, and the rest of the time Muse needs the memory.
static void install_task(void *arg)
{
    (void)arg;
    char id[ID_LEN];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(id, sizeof(id), "%s", s_offer);
    const int size = s_offer_size;
    xSemaphoreGive(s_lock);
    if (install(id, size)) {
        vTaskDelay(pdMS_TO_TICKS(1500));  // let the last message out
        esp_restart();
    }
    s_next_try_ms = muse_now_ms() + RETRY_AFTER_MS;
    if (++s_tries >= TRIES) {
        ESP_LOGW(TAG, "giving up on this update until the server offers one again");
        s_offer_due = false;
        s_tries = 0;
    }
    s_busy = false;
    vTaskDelete(NULL);
}

// Called a few times a second by the companion task.
void update_poll(void)
{
    if (!s_lock || s_busy) {
        return;
    }
    const int64_t now = muse_now_ms();
    if (s_refused_due) {
        s_refused_due = false;
        report("refused", 0, "this update did not work before");
    }
    if (s_on_trial) {
        if (s_linked) {
            esp_ota_mark_app_valid_cancel_rollback();
            s_on_trial = false;
            ESP_LOGI(TAG, "the new firmware works: keeping it");
            report("kept", 100, NULL);
        } else if (now > CONFIRM_WITHIN_MS) {
            ESP_LOGW(TAG, "the new firmware could not link to a server; going back to the one before");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        return;
    }
    if (!s_offer_due || now < s_next_try_ms) {
        return;
    }
    // Only when Muse has been left alone for a little while.
    const int state = g_muse.agent_state;
    const bool quiet = (state == MUSE_AGENT_IDLE || state == MUSE_AGENT_OFF) && !g_muse.touch_down &&
                       !g_muse.button_down && !g_muse.setup_active && companion_connected();
    if (!quiet) {
        s_quiet_since = 0;
        return;
    }
    if (!s_quiet_since) {
        s_quiet_since = now;
    }
    if (now - s_quiet_since < QUIET_MS) {
        return;
    }
    s_quiet_since = 0;
    s_busy = true;
    if (xTaskCreatePinnedToCore(install_task, "muse_update", 8192, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGW(TAG, "no memory to start the update; trying again later");
        s_busy = false;
        s_next_try_ms = now + RETRY_AFTER_MS;
    }
}

// ---------------------------------------------------------------- API
void update_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    const esp_partition_t *running = esp_ota_get_running_partition();
    partition_id(running, s_running);
    snprintf(s_version, sizeof(s_version), "%s", esp_app_get_description()->version);
    partition_id(esp_ota_get_last_invalid_partition(), s_failed);
    esp_ota_img_states_t state;
    s_on_trial = running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
                 state == ESP_OTA_IMG_PENDING_VERIFY;
    ESP_LOGI(TAG, "firmware %s (%.8s) in %s%s", s_version, s_running, running ? running->label : "?",
             s_on_trial ? ", on trial until it links to a server" : "");
    if (s_failed[0]) {
        ESP_LOGW(TAG, "an update (%.8s) was dropped after it did not work", s_failed);
    }
}

const char *update_running_id(void)
{
    return s_running;
}

const char *update_running_version(void)
{
    return s_version;
}

void update_offer(const char *id, int size)
{
    if (!s_lock || !id || strlen(id) != ID_LEN - 1 || strcmp(id, s_running) == 0) {
        return;
    }
    if (strcmp(id, s_failed) == 0) {
        ESP_LOGW(TAG, "the server offers %.8s, which did not work before; not installing it again", id);
        s_refused_due = true;  // the task tells the server (this runs in the websocket's own task)
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool is_new = strcmp(id, s_offer) != 0;
    snprintf(s_offer, sizeof(s_offer), "%s", id);
    s_offer_size = size;
    xSemaphoreGive(s_lock);
    if (is_new || !s_offer_due) {
        ESP_LOGI(TAG, "the server has an update (%.8s, %d KB); installing it when Muse is left alone", id, size / 1024);
    }
    s_offer_due = true;
}

void update_on_linked(void)
{
    s_linked = true;
}

bool update_busy(void)
{
    return s_busy;
}
