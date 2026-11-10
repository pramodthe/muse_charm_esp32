// SPDX-License-Identifier: MIT
// On-screen setup: pick a WiFi network and type its password on Muse's own
// touch screen. (The Deepgram key for voice chat lives on your computer, with
// the companion server.)
//
//   Wi-Fi list -> password -> all set
//
// The keyboard follows the smartwatch style: no key borders, large letters,
// every pixel of a row belongs to a key, the key is chosen when the finger
// lifts (so you can slide to correct it) and the pressed letter pops up above
// the finger. Nothing tappable sits in the screen's rounded corners.
//
// The LVGL callbacks only change screens; scanning and connecting run in a
// small worker task so the display never stalls.

#include "setup_ui.h"

#include <stdio.h>
#include <string.h>

#include "behavior.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "muse.h"
#include "wifi.h"

static const char *TAG = "setup";

#define SCREEN_W 368
#define SCREEN_H 448
#define SAFE_X 34          // keep taps out of the rounded corners
#define KEYBOARD_H 236
#define MAX_NETWORKS 12
#define COL_ACCENT 0x5E9BFF
#define COL_TEXT 0xFFFFFF
#define COL_DIM 0x8E8E93
#define COL_ERROR 0xFF6961
#define COL_PRESSED 0x3A3A3C
#define COL_ROW 0x1C1C1E

typedef enum { CMD_SCAN, CMD_CONNECT, CMD_CLOSE } cmd_t;
typedef enum { KB_LOWER, KB_UPPER, KB_NUMBERS, KB_SYMBOLS } kb_mode_t;

static volatile bool s_active;
static QueueHandle_t s_cmds;
static lv_obj_t *s_screen, *s_previous, *s_page, *s_status, *s_list, *s_text, *s_keys;
static kb_mode_t s_mode;
static bool s_shift_once;
static char s_ssid[33], s_pass[65];
static wifi_auth_mode_t s_auth;
static wifi_ap_record_t s_found[MAX_NETWORKS];
static int s_found_count;
static bool s_scanned;

static void step_wifi(void);
static void step_password(void);
static void step_done(void);

// ---------------------------------------------------------------- keyboard
#define K (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_POPOVER)
#define F (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG)
#define GAP (LV_BUTTONMATRIX_CTRL_HIDDEN | LV_BUTTONMATRIX_CTRL_DISABLED)
#define SPACE "space"
#define SHIFT LV_SYMBOL_UP

static const char *const MAP_LOWER[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    " ", "a", "s", "d", "f", "g", "h", "j", "k", "l", " ", "\n",
    SHIFT, "z", "x", "c", "v", "b", "n", "m", "123", "\n",
    " ", SPACE, " ", "",
};
static const char *const MAP_UPPER[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    " ", "A", "S", "D", "F", "G", "H", "J", "K", "L", " ", "\n",
    SHIFT, "Z", "X", "C", "V", "B", "N", "M", "123", "\n",
    " ", SPACE, " ", "",
};
static const lv_buttonmatrix_ctrl_t CTRL_LETTERS[] = {
    K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2,
    GAP | 1, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, GAP | 1,
    F | 3, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, F | 3,
    GAP | 2, F | 5, GAP | 2,
};
static const char *const MAP_NUMBERS[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
    "#+=", ".", ",", "?", "!", "'", "_", "abc", "\n",
    " ", SPACE, " ", "",
};
static const char *const MAP_SYMBOLS[] = {
    "[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n",
    "~", "\\", "|", "<", ">", "`", ";", ":", "'", "\"", "\n",
    "123", ".", ",", "?", "!", "'", "_", "abc", "\n",
    " ", SPACE, " ", "",
};
static const lv_buttonmatrix_ctrl_t CTRL_SIGNS[] = {
    K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2,
    K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2,
    F | 3, K | 2, K | 2, K | 2, K | 2, K | 2, K | 2, F | 3,
    GAP | 2, F | 5, GAP | 2,
};

static void keyboard_set_mode(kb_mode_t mode)
{
    s_mode = mode;
    switch (mode) {
    case KB_LOWER:
        lv_buttonmatrix_set_map(s_keys, MAP_LOWER);
        lv_buttonmatrix_set_ctrl_map(s_keys, CTRL_LETTERS);
        break;
    case KB_UPPER:
        lv_buttonmatrix_set_map(s_keys, MAP_UPPER);
        lv_buttonmatrix_set_ctrl_map(s_keys, CTRL_LETTERS);
        break;
    case KB_NUMBERS:
        lv_buttonmatrix_set_map(s_keys, MAP_NUMBERS);
        lv_buttonmatrix_set_ctrl_map(s_keys, CTRL_SIGNS);
        break;
    case KB_SYMBOLS:
        lv_buttonmatrix_set_map(s_keys, MAP_SYMBOLS);
        lv_buttonmatrix_set_ctrl_map(s_keys, CTRL_SIGNS);
        break;
    }
}

static void on_key(lv_event_t *e)
{
    (void)e;
    const uint32_t id = lv_buttonmatrix_get_selected_button(s_keys);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) {
        return;
    }
    const char *txt = lv_buttonmatrix_get_button_text(s_keys, id);
    if (!txt || strcmp(txt, " ") == 0) {
        return;
    }
    if (strcmp(txt, SHIFT) == 0) {
        s_shift_once = s_mode == KB_LOWER;
        keyboard_set_mode(s_mode == KB_LOWER ? KB_UPPER : KB_LOWER);
    } else if (strcmp(txt, "123") == 0) {
        keyboard_set_mode(KB_NUMBERS);
    } else if (strcmp(txt, "#+=") == 0) {
        keyboard_set_mode(KB_SYMBOLS);
    } else if (strcmp(txt, "abc") == 0) {
        keyboard_set_mode(KB_LOWER);
    } else {
        lv_textarea_add_text(s_text, strcmp(txt, SPACE) == 0 ? " " : txt);
        if (s_mode == KB_UPPER && s_shift_once) {
            s_shift_once = false;
            keyboard_set_mode(KB_LOWER);  // one capital, like a phone
        }
    }
}

static void on_backspace(lv_event_t *e)
{
    (void)e;
    lv_textarea_delete_char(s_text);
}

static lv_obj_t *keyboard_create(lv_obj_t *parent, kb_mode_t mode)
{
    s_keys = lv_buttonmatrix_create(parent);
    lv_obj_remove_style_all(s_keys);
    lv_obj_set_size(s_keys, SCREEN_W - 8, KEYBOARD_H);
    lv_obj_align(s_keys, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_pad_all(s_keys, 0, 0);
    lv_obj_set_style_pad_row(s_keys, 0, 0);
    lv_obj_set_style_pad_column(s_keys, 0, 0);
    lv_obj_set_style_bg_opa(s_keys, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_keys, lv_color_hex(COL_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_keys, &lv_font_montserrat_24, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_keys, 14, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keys, lv_color_hex(COL_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_keys, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_keys, lv_color_hex(COL_DIM), LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_keys, on_key, LV_EVENT_VALUE_CHANGED, NULL);
    s_shift_once = false;
    keyboard_set_mode(mode);
    return s_keys;
}

// ---------------------------------------------------------------- page parts
static void send_cmd(cmd_t cmd)
{
    xQueueSend(s_cmds, &cmd, 0);
}

static lv_obj_t *text_label(lv_obj_t *parent, const char *text, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font, 0);
    return l;
}

// A text button with a generous invisible touch area.
static lv_obj_t *text_button(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(b, 10, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_t *l = text_label(b, text, color, &lv_font_montserrat_20);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void set_status(const char *text, uint32_t color)
{
    if (s_status) {
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
    }
}

// Start a fresh page: top bar with a left and right action, and a title.
// The old page may hold the object whose event got us here, so it is
// deleted asynchronously.
static lv_obj_t *new_page(const char *title, const char *left, lv_event_cb_t on_left, const char *right,
                          lv_event_cb_t on_right)
{
    if (s_page) {
        lv_obj_delete_async(s_page);
    }
    s_page = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, SCREEN_W, SCREEN_H);
    s_status = s_list = s_text = s_keys = NULL;
    if (left) {
        lv_obj_align(text_button(s_page, left, COL_TEXT, on_left), LV_ALIGN_TOP_LEFT, SAFE_X - 10, 14);
    }
    if (right) {
        lv_obj_align(text_button(s_page, right, COL_ACCENT, on_right), LV_ALIGN_TOP_RIGHT, -(SAFE_X - 10), 14);
    }
    lv_obj_t *t = text_label(s_page, title, COL_DIM, &lv_font_montserrat_14);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, SCREEN_W - 2 * SAFE_X);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 64);
    return s_page;
}

// Text line with a backspace button, a status line, and the keyboard.
static void entry_area(lv_obj_t *page, const char *placeholder, kb_mode_t mode, const lv_font_t *font)
{
    s_text = lv_textarea_create(page);
    lv_obj_remove_style_all(s_text);
    lv_textarea_set_one_line(s_text, true);
    lv_textarea_set_max_length(s_text, 64);
    lv_textarea_set_placeholder_text(s_text, placeholder);
    lv_obj_set_size(s_text, SCREEN_W - 2 * 20 - 56, 44);
    lv_obj_align(s_text, LV_ALIGN_TOP_LEFT, 20, 92);
    lv_obj_set_style_text_font(s_text, font, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(COL_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_pad_top(s_text, 8, 0);
    lv_obj_set_style_border_color(s_text, lv_color_hex(COL_ACCENT), LV_PART_CURSOR);
    lv_obj_set_style_border_width(s_text, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_side(s_text, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_add_state(s_text, LV_STATE_FOCUSED);  // show the cursor

    lv_obj_t *del = lv_button_create(page);
    lv_obj_remove_style_all(del);
    lv_obj_set_size(del, 56, 44);
    lv_obj_align(del, LV_ALIGN_TOP_RIGHT, -20, 92);
    lv_obj_set_style_radius(del, 12, 0);
    lv_obj_set_style_bg_color(del, lv_color_hex(COL_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(del, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *icon = text_label(del, LV_SYMBOL_BACKSPACE, COL_TEXT, &lv_font_montserrat_24);
    lv_obj_center(icon);
    lv_obj_add_event_cb(del, on_backspace, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(del, on_backspace, LV_EVENT_LONG_PRESSED_REPEAT, NULL);

    lv_obj_t *line = lv_obj_create(page);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, SCREEN_W - 40, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 138);

    s_status = text_label(page, "", COL_DIM, &lv_font_montserrat_14);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_status, SCREEN_W - 40);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 148);

    keyboard_create(page, mode);
}

// ---------------------------------------------------------------- closing
static void close_setup(lv_event_t *e)
{
    (void)e;
    s_active = false;
    lv_screen_load(s_previous);
    lv_obj_delete_async(s_screen);
    s_screen = s_page = NULL;
    send_cmd(CMD_CLOSE);
    behavior_post(MUSE_EV_TAP);  // say hi
    ESP_LOGI(TAG, "closed");
}

static void use_phone(lv_event_t *e)
{
    close_setup(e);
    wifi_open_setup();
}

// ---------------------------------------------------------------- Wi-Fi list
static bool is_enterprise(wifi_auth_mode_t m)
{
    return m == WIFI_AUTH_ENTERPRISE || m == WIFI_AUTH_WPA3_ENTERPRISE || m == WIFI_AUTH_WPA2_WPA3_ENTERPRISE ||
           m == WIFI_AUTH_WPA3_ENT_192 || m == WIFI_AUTH_WPA_ENTERPRISE;
}

static void on_network(lv_event_t *e)
{
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    strlcpy(s_ssid, (const char *)s_found[i].ssid, sizeof(s_ssid));
    s_auth = s_found[i].authmode;
    ESP_LOGI(TAG, "chose %s (security %d, signal %d dBm)", s_ssid, s_found[i].authmode, s_found[i].rssi);
    if (is_enterprise(s_found[i].authmode)) {
        set_status("That network needs a username (enterprise login), so Muse can't join it.", COL_ERROR);
        return;
    }
    if (s_found[i].authmode == WIFI_AUTH_OPEN) {
        s_pass[0] = '\0';
        set_status("Connecting...", COL_DIM);
        send_cmd(CMD_CONNECT);
    } else {
        step_password();
    }
}

static void keep_wifi(lv_event_t *e)
{
    (void)e;
    step_done();
}

static lv_obj_t *list_row(const char *icon, const char *text, uint32_t color, lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_button_create(s_list);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), 52);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_ROW), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    if (icon) {
        text_label(b, icon, COL_DIM, &lv_font_montserrat_20);
    }
    lv_obj_t *l = text_label(b, text, color, &lv_font_montserrat_20);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    return b;
}

static void fill_list(void)
{
    if (!s_list) {
        return;
    }
    lv_obj_clean(s_list);
    char current[33];
    if (wifi_connected() && wifi_current_ssid(current, sizeof(current))) {
        char text[48];
        snprintf(text, sizeof(text), "Keep %s", current);
        list_row(LV_SYMBOL_OK, text, COL_ACCENT, keep_wifi, NULL);
    }
    for (int i = 0; i < s_found_count; i++) {
        list_row(LV_SYMBOL_WIFI, (const char *)s_found[i].ssid, COL_TEXT, on_network, (void *)(intptr_t)i);
    }
    list_row(NULL, "Set up from a phone instead", COL_DIM, use_phone, NULL);
    // Room at the bottom so the last rows can scroll clear of the rounded corners.
    lv_obj_t *pad = lv_obj_create(s_list);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 1, 30);
    set_status(s_found_count ? "Tap your network" : "No networks found", s_found_count ? COL_DIM : COL_ERROR);
}

static void rescan(lv_event_t *e)
{
    (void)e;
    if (s_list) {
        lv_obj_clean(s_list);
    }
    set_status("Looking for Wi-Fi...", COL_DIM);
    send_cmd(CMD_SCAN);
}

static void step_wifi(void)
{
    lv_obj_t *page = new_page("Choose Wi-Fi", "Later", close_setup, LV_SYMBOL_REFRESH, rescan);
    s_status = text_label(page, "Looking for Wi-Fi...", COL_DIM, &lv_font_montserrat_14);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 86);
    s_list = lv_obj_create(page);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, SCREEN_W - 2 * 20, SCREEN_H - 112 - 16);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    if (s_scanned) {
        fill_list();
    } else {
        send_cmd(CMD_SCAN);
    }
}

// ---------------------------------------------------------------- password
static void join(lv_event_t *e)
{
    (void)e;
    strlcpy(s_pass, lv_textarea_get_text(s_text), sizeof(s_pass));
    if (s_auth != WIFI_AUTH_WEP && strlen(s_pass) < 8) {
        set_status("Wi-Fi passwords are at least 8 characters", COL_ERROR);
        return;
    }
    set_status("Connecting...", COL_DIM);
    send_cmd(CMD_CONNECT);
}

static void back_to_list(lv_event_t *e)
{
    (void)e;
    step_wifi();
}

static void step_password(void)
{
    char title[64];
    snprintf(title, sizeof(title), "Password for %s", s_ssid);
    lv_obj_t *page = new_page(title, "Cancel", back_to_list, "Join", join);
    entry_area(page, "Password", KB_LOWER, &lv_font_montserrat_24);
}

// ---------------------------------------------------------------- done
static void step_done(void)
{
    lv_obj_t *page = new_page("All set", NULL, NULL, NULL, NULL);
    lv_obj_t *msg = text_label(page,
                               "Muse is on Wi-Fi.\nRun the Muse companion on your computer to talk to Muse.",
                               COL_TEXT, &lv_font_montserrat_20);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(msg, SCREEN_W - 2 * SAFE_X);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t *done = lv_button_create(page);
    lv_obj_remove_style_all(done);
    lv_obj_set_size(done, 200, 60);
    lv_obj_set_style_radius(done, 30, 0);
    lv_obj_set_style_bg_color(done, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(done, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(done, lv_color_hex(0x3D7BE0), LV_STATE_PRESSED);
    lv_obj_align(done, LV_ALIGN_CENTER, 0, 70);
    lv_obj_t *l = text_label(done, "Done", COL_TEXT, &lv_font_montserrat_24);
    lv_obj_center(l);
    lv_obj_add_event_cb(done, close_setup, LV_EVENT_CLICKED, NULL);
}

// ---------------------------------------------------------------- worker
// Keep the strongest entry per network name, strongest first.
static void tidy_networks(wifi_ap_record_t *aps, int n)
{
    s_found_count = 0;
    for (int i = 0; i < n; i++) {
        if (aps[i].ssid[0] == '\0') {
            continue;
        }
        int j = 0;
        for (; j < s_found_count; j++) {
            if (strcmp((const char *)s_found[j].ssid, (const char *)aps[i].ssid) == 0) {
                break;
            }
        }
        if (j < s_found_count) {
            if (aps[i].rssi > s_found[j].rssi) {
                s_found[j] = aps[i];
            }
        } else if (s_found_count < MAX_NETWORKS) {
            s_found[s_found_count++] = aps[i];
        }
    }
    for (int i = 1; i < s_found_count; i++) {
        for (int j = i; j > 0 && s_found[j].rssi > s_found[j - 1].rssi; j--) {
            const wifi_ap_record_t t = s_found[j];
            s_found[j] = s_found[j - 1];
            s_found[j - 1] = t;
        }
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    static wifi_ap_record_t aps[24];
    wifi_hold(true);
    for (;;) {
        cmd_t cmd;
        xQueueReceive(s_cmds, &cmd, portMAX_DELAY);
        if (cmd == CMD_CLOSE) {
            break;
        }
        if (cmd == CMD_SCAN) {
            const int n = wifi_scan(aps, 24);
            if (bsp_display_lock(0)) {
                tidy_networks(aps, n);
                s_scanned = true;
                if (s_active) {
                    fill_list();
                }
                bsp_display_unlock();
            }
        } else if (cmd == CMD_CONNECT) {
            const bool ok = wifi_try_connect(s_ssid, s_pass, 15000);
            if (bsp_display_lock(0)) {
                if (s_active && ok) {
                    step_done();
                } else if (s_active) {
                    set_status("Couldn't join. Check the password and try again.", COL_ERROR);
                }
                bsp_display_unlock();
            }
        }
    }
    wifi_hold(false);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- public API
void setup_ui_open(void)
{
    if (s_active) {
        return;
    }
    if (!s_cmds) {
        s_cmds = xQueueCreate(4, sizeof(cmd_t));
    }
    xQueueReset(s_cmds);
    s_active = true;
    s_scanned = false;
    if (xTaskCreatePinnedToCore(worker_task, "muse_setup", 5 * 1024, NULL, 3, NULL, 0) != pdPASS) {
        s_active = false;
        return;
    }
    if (bsp_display_lock(0)) {
        s_previous = lv_screen_active();
        s_screen = lv_obj_create(NULL);
        lv_obj_remove_style_all(s_screen);
        lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        s_page = NULL;
        step_wifi();
        lv_screen_load(s_screen);
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "opened");
}

bool setup_ui_active(void)
{
    return s_active;
}
