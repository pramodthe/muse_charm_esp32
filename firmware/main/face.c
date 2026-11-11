// SPDX-License-Identifier: MIT
// Live face: eyes and mouth drawn with LVGL on top of the body frames.
//
// The body frames are rendered without a face. Each frame carries anchors
// (eye and mouth centres, face roll and a suggested expression) exported from
// Blender, and this module draws the face there. Because the face is live it
// can follow a finger, blink and change expression at 30 fps while the body
// plays at 10 fps.
//
// The face is laid out through the camera's view (camera.c): positions move
// and sizes grow with the zoom, so in a close-up the body is an enlarged
// picture but the eyes and mouth are drawn sharp at their new size.

#include "face.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "camera.h"
#include "clips.h"
#include "esp_random.h"
#include "muse.h"
#include "wifi.h"

#define GAZE_X_PX 3.5f
#define GAZE_Y_PX 2.5f
#define FACE_TICK_MS 33

#define COL_EYE 0x3a2f29
#define COL_HIGHLIGHT 0xfff6ee
#define COL_LINE 0x4a3830
#define COL_MOUTH 0x5a2a28
#define COL_TONGUE 0xe07a80
#define COL_CAPTION 0xF4EEE8
#define COL_ZZZ 0xD8D0C8
#define COL_CLOUD 0xF4EEE8
#define COL_CLOUD_DOT 0xC4BAB0

// One face object, with its last layout cached so unchanged parts are not
// invalidated (and redrawn) every tick.
typedef struct {
    lv_obj_t *obj;
    int x, y, w, h, rot, stroke;
    bool shown;
} part_t;

static part_t s_eye[2], s_highlight[2], s_happy[2], s_squint[2], s_dizzy[2], s_asleep[2];
static part_t s_smile, s_open_mouth, s_tongue, s_round_mouth;
static lv_point_precise_t s_squint_pts[2][3];
static int s_squint_zoom = -1;  // the zoom (x16) the squint lines were laid out for
static lv_obj_t *s_setup_hint;
static lv_obj_t *s_caption;
static lv_obj_t *s_zzz;
static int s_zzz_step = -1;
// The thought cloud: body, two bumps on top, two bubbles trailing down toward
// the head, and three dots that light up in turn.
static part_t s_cloud[5], s_cloud_dot[3];
static int s_cloud_lit = -1;
static lv_obj_t *s_sign;
static int64_t s_sign_until_ms;  // LVGL lock
static char s_caption_text[64];  // what the caption shows (behaviour task)
static bool s_caption_on;        // caption has text (LVGL lock)

static muse_view_t s_view = {.zoom = 1.0f};  // the camera's view on screen
static float s_gx, s_gy;      // smoothed gaze, -1..1
static int s_blink_tick = -1;  // -1 while the eyes are open
static int64_t s_next_blink_ms, s_next_glance_ms;
static float s_glance_x, s_glance_y;
static int s_spin;

static void part_hide(part_t *p)
{
    if (p->shown) {
        lv_obj_add_flag(p->obj, LV_OBJ_FLAG_HIDDEN);
        p->shown = false;
    }
}

// Show a part centred on screen point (cx, cy).
static void part_show(part_t *p, float cx, float cy, int w, int h)
{
    const int x = (int)lroundf(cx - w * 0.5f);
    const int y = (int)lroundf(cy - h * 0.5f);
    if (w != p->w || h != p->h) {
        lv_obj_set_size(p->obj, w, h);
        p->w = w;
        p->h = h;
    }
    if (x != p->x || y != p->y) {
        lv_obj_set_pos(p->obj, x, y);
        p->x = x;
        p->y = y;
    }
    if (!p->shown) {
        lv_obj_remove_flag(p->obj, LV_OBJ_FLAG_HIDDEN);
        p->shown = true;
    }
}

static void arc_rotate(part_t *p, int rot)
{
    if (rot != p->rot) {
        lv_arc_set_rotation(p->obj, rot);
        p->rot = rot;
    }
}

static void arc_stroke(part_t *p, int width)
{
    if (width != p->stroke) {
        lv_obj_set_style_arc_width(p->obj, width, LV_PART_MAIN);
        p->stroke = width;
    }
}

static void part_init(part_t *p, lv_obj_t *obj)
{
    p->obj = obj;
    p->x = p->y = p->w = p->h = p->rot = p->stroke = INT32_MIN;
    p->shown = true;
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    part_hide(p);
}

static lv_obj_t *make_blob(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    return o;
}

// An arc drawn with the background part only, between start and end degrees
// (0 = 3 o'clock, clockwise).
static lv_obj_t *make_arc(lv_obj_t *parent, uint32_t color, bool rounded, int start, int end)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, rounded, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_arc_set_bg_angles(a, start, end);
    lv_arc_set_rotation(a, 0);
    return a;
}

static lv_obj_t *make_line(lv_obj_t *parent, uint32_t color, lv_point_precise_t *pts)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_line_set_points(l, pts, 3);
    return l;
}

// (dx, dy) rotated by the face roll.
static void roll_offset(float roll_rad, float dx, float dy, float *ox, float *oy)
{
    const float c = cosf(roll_rad), s = sinf(roll_rad);
    *ox = dx * c - dy * s;
    *oy = dx * s + dy * c;
}

// A length in frame pixels as screen pixels at the current zoom.
static int zs(float px)
{
    const int v = (int)lroundf(px * s_view.zoom);
    return v < 1 ? 1 : v;
}

// ">" for the left eye and "<" for the right eye, in a 10 x 13 box (frame
// pixels), laid out again whenever the zoom changes.
static void squint_layout(void)
{
    const int zoom16 = (int)lroundf(s_view.zoom * 16.0f);
    if (zoom16 == s_squint_zoom) {
        return;
    }
    s_squint_zoom = zoom16;
    const float z = s_view.zoom;
    for (int i = 0; i < 2; i++) {
        const float outer = i == 0 ? 2 : 8, inner = i == 0 ? 8 : 2;
        s_squint_pts[i][0] = (lv_point_precise_t){(int)lroundf(outer * z), (int)lroundf(2 * z)};
        s_squint_pts[i][1] = (lv_point_precise_t){(int)lroundf(inner * z), (int)lroundf(6 * z)};
        s_squint_pts[i][2] = (lv_point_precise_t){(int)lroundf(outer * z), (int)lroundf(11 * z)};
        lv_obj_set_style_line_width(s_squint[i].obj, zs(3), 0);
        lv_line_set_points(s_squint[i].obj, s_squint_pts[i], 3);
    }
}

void face_apply(void)
{
    camera_view(&s_view);
    const float z = s_view.zoom;
    const muse_frames_header_t *hdr = clips_header();
    const muse_anchor_t *a = clips_anchor(g_muse.frame);
    // Anchors are in frame pixels x4; bring them to the screen through the view.
    const float ex[2] = {(a->eye_l_x * 0.25f - s_view.ox) * z, (a->eye_r_x * 0.25f - s_view.ox) * z};
    const float ey[2] = {(a->eye_l_y * 0.25f - s_view.oy) * z, (a->eye_r_y * 0.25f - s_view.oy) * z};
    const float mx = (a->mouth_x * 0.25f - s_view.ox) * z, my = (a->mouth_y * 0.25f - s_view.oy) * z;
    const float roll_deg = a->roll10 * 0.1f;
    const float roll = roll_deg * (float)M_PI / 180.0f;
    const int roll_i = (int)lroundf(roll_deg);
    const int expr = g_muse.expr_override >= 0 ? g_muse.expr_override : a->expr;
    const float gx = s_gx * GAZE_X_PX * z, gy = s_gy * GAZE_Y_PX * z;
    float ox, oy;

    // Eyes
    for (int i = 0; i < 2; i++) {
        if (expr == MUSE_EXPR_HAPPY) {
            roll_offset(roll, 0, 4.0f * z, &ox, &oy);
            part_show(&s_happy[i], ex[i] + ox, ey[i] + oy, zs(13), zs(13));
            arc_stroke(&s_happy[i], zs(3));
            arc_rotate(&s_happy[i], roll_i);
        } else {
            part_hide(&s_happy[i]);
        }

        if (expr == MUSE_EXPR_SLEEP) {
            roll_offset(roll, 0, -3.0f * z, &ox, &oy);
            part_show(&s_asleep[i], ex[i] + ox, ey[i] + oy, zs(13), zs(13));
            arc_stroke(&s_asleep[i], zs(3));
            arc_rotate(&s_asleep[i], roll_i);
        } else {
            part_hide(&s_asleep[i]);
        }

        if (expr == MUSE_EXPR_SQUEEZE) {
            squint_layout();
            part_show(&s_squint[i], ex[i], ey[i], zs(10), zs(13));
        } else {
            part_hide(&s_squint[i]);
        }

        if (expr == MUSE_EXPR_DIZZY) {
            part_show(&s_dizzy[i], ex[i], ey[i], zs(12), zs(12));
            arc_stroke(&s_dizzy[i], zs(2));
            arc_rotate(&s_dizzy[i], i == 0 ? s_spin : 360 - s_spin);
        } else {
            part_hide(&s_dizzy[i]);
        }

        if (expr == MUSE_EXPR_NORMAL || expr == MUSE_EXPR_TALK || expr == MUSE_EXPR_SURPRISED) {
            static const int8_t blink_h[] = {13, 8, 2, 2, 8};
            float w = hdr->eye_w, h = hdr->eye_h;
            if (expr == MUSE_EXPR_SURPRISED) {
                w += 2;
                h += 2;
            }
            if (s_blink_tick >= 0) {
                h = blink_h[s_blink_tick] * h / 13.0f;
                w += (h < 6) ? 2 : 0;
            }
            part_show(&s_eye[i], ex[i] + gx, ey[i] + gy, zs(w), zs(h));
            if (h >= 8) {
                part_show(&s_highlight[i], ex[i] + gx - w * z * 0.22f, ey[i] + gy - h * z * 0.25f, zs(3), zs(3));
            } else {
                part_hide(&s_highlight[i]);
            }
        } else {
            part_hide(&s_eye[i]);
            part_hide(&s_highlight[i]);
        }
    }

    // Mouth
    if (expr == MUSE_EXPR_NORMAL || expr == MUSE_EXPR_SLEEP) {
        roll_offset(roll, 0, -6.0f * z, &ox, &oy);
        part_show(&s_smile, mx + ox, my + oy, zs(20), zs(20));
        arc_stroke(&s_smile, zs(3));
        arc_rotate(&s_smile, roll_i);
    } else {
        part_hide(&s_smile);
    }

    if (expr == MUSE_EXPR_HAPPY || expr == MUSE_EXPR_SQUEEZE) {
        roll_offset(roll, 0, -2.5f * z, &ox, &oy);
        part_show(&s_open_mouth, mx + ox, my + oy, zs(18), zs(18));
        arc_stroke(&s_open_mouth, zs(9));
        arc_rotate(&s_open_mouth, roll_i);
        roll_offset(roll, 0, 3.5f * z, &ox, &oy);
        part_show(&s_tongue, mx + ox, my + oy, zs(8), zs(4));
    } else {
        part_hide(&s_open_mouth);
        part_hide(&s_tongue);
    }

    if (expr == MUSE_EXPR_TALK || expr == MUSE_EXPR_DIZZY || expr == MUSE_EXPR_SURPRISED) {
        float w = 7, h = 7;
        if (expr == MUSE_EXPR_TALK) {
            const float level = g_muse.mouth_level;
            w = 9.0f + 8.0f * level;
            h = 3.0f + 15.0f * level;
        } else if (expr == MUSE_EXPR_DIZZY) {
            w = 8 + (s_spin / 90) % 2 * 2;
            h = 6;
        }
        part_show(&s_round_mouth, mx, my + z, zs(w), zs(h));
    } else {
        part_hide(&s_round_mouth);
    }

    // A thought cloud above the head while Muse thinks.
    if (g_muse.thought) {
        const int bw = zs(62), bh = zs(30);
        const float cx = muse_clampf((ex[0] + ex[1]) * 0.5f + 67.0f * z, bw * 0.5f + 8.0f,
                                     hdr->width - bw * 0.5f - 8.0f);
        const float cy = fmaxf((ey[0] + ey[1]) * 0.5f - 94.0f * z, bh * 0.5f + zs(13) + 6.0f);
        part_show(&s_cloud[0], cx - bw * 0.18f, cy - bh * 0.38f, zs(24), zs(24));
        part_show(&s_cloud[1], cx + bw * 0.12f, cy - bh * 0.45f, zs(30), zs(30));
        part_show(&s_cloud[2], cx, cy, bw, bh);
        part_show(&s_cloud[3], cx - 2.0f * z, cy + 30.0f * z, zs(9), zs(9));
        part_show(&s_cloud[4], cx + 2.0f * z, cy + 43.0f * z, zs(6), zs(6));
        const int lit = (int)((muse_now_ms() / 350) % 3);
        for (int i = 0; i < 3; i++) {
            const int d = i == lit ? zs(8) : zs(6);
            part_show(&s_cloud_dot[i], cx + (i - 1) * bw * 0.22f, cy, d, d);
        }
        if (lit != s_cloud_lit) {
            s_cloud_lit = lit;
            for (int i = 0; i < 3; i++) {
                lv_obj_set_style_bg_color(s_cloud_dot[i].obj, lv_color_hex(i == lit ? COL_LINE : COL_CLOUD_DOT), 0);
            }
        }
    } else {
        for (int i = 0; i < 5; i++) {
            part_hide(&s_cloud[i]);
        }
        for (int i = 0; i < 3; i++) {
            part_hide(&s_cloud_dot[i]);
        }
    }

    // "z z Z" drifting up beside the head while Muse sleeps.
    const bool asleep = expr == MUSE_EXPR_SLEEP;
    if (asleep == lv_obj_has_flag(s_zzz, LV_OBJ_FLAG_HIDDEN)) {
        if (asleep) {
            lv_obj_remove_flag(s_zzz, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_zzz, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (asleep) {
        static const char *const steps[] = {"", "z", "z z", "z z Z"};
        const int step = (int)((muse_now_ms() / 900) % 4);
        if (step != s_zzz_step) {
            s_zzz_step = step;
            lv_label_set_text(s_zzz, steps[step]);
        }
        // Just off the head's upper right.
        static int last_x = INT32_MIN, last_y = INT32_MIN;
        roll_offset(roll, 58.0f * z, -78.0f * z, &ox, &oy);
        const int x = (int)lroundf((ex[0] + ex[1]) * 0.5f + ox), y = (int)lroundf((ey[0] + ey[1]) * 0.5f + oy);
        if (x != last_x || y != last_y) {
            lv_obj_set_pos(s_zzz, x, y);
            last_x = x;
            last_y = y;
        }
    }
}

// Gaze smoothing, blinks, idle glances and the dizzy spin. Runs in the LVGL
// task, so the lock is already held.
volatile uint32_t g_face_ticks;  // display liveness, watched by main.c

static void face_tick(lv_timer_t *t)
{
    (void)t;
    g_face_ticks++;
    const int64_t now = muse_now_ms();
    float tx, ty;
    if (g_muse.touch_down) {
        // Look at the finger.
        const muse_anchor_t *a = clips_anchor(g_muse.frame);
        const float cx = (a->eye_l_x + a->eye_r_x) * 0.125f;
        const float cy = (a->eye_l_y + a->eye_r_y) * 0.125f;
        tx = muse_clampf((g_muse.touch_fx - cx) / 90.0f, -1.0f, 1.0f);
        ty = muse_clampf((g_muse.touch_fy - cy) / 110.0f, -1.0f, 1.0f);
    } else if (g_muse.gaze_mode == MUSE_GAZE_TARGET) {
        tx = g_muse.gaze_x;
        ty = g_muse.gaze_y;
    } else {
        if (now >= s_next_glance_ms) {
            const uint32_t r = esp_random();
            if (r % 10 < 3) {
                s_glance_x = 0;
                s_glance_y = 0;
            } else {
                s_glance_x = ((int)(r % 141) - 70) / 100.0f;
                s_glance_y = ((int)((r >> 8) % 81) - 40) / 100.0f;
            }
            s_next_glance_ms = now + 1200 + (esp_random() % 2800);
        }
        tx = s_glance_x;
        ty = s_glance_y;
    }
    s_gx += (tx - s_gx) * 0.45f;
    s_gy += (ty - s_gy) * 0.45f;

    if (s_blink_tick >= 0) {
        if (++s_blink_tick >= 5) {
            s_blink_tick = -1;
        }
    } else if (g_muse.blink_request || now >= s_next_blink_ms) {
        g_muse.blink_request = false;
        s_blink_tick = 0;
        s_next_blink_ms = now + 2200 + (esp_random() % 4500);
        if (esp_random() % 6 == 0) {
            s_next_blink_ms = now + 260;  // sometimes a double blink
        }
    }
    s_spin = (s_spin + 24) % 360;
    if (g_muse.setup_active == lv_obj_has_flag(s_setup_hint, LV_OBJ_FLAG_HIDDEN)) {
        if (g_muse.setup_active) {
            lv_obj_remove_flag(s_setup_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_setup_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_sign_until_ms && now >= s_sign_until_ms) {
        s_sign_until_ms = 0;
        lv_obj_add_flag(s_sign, LV_OBJ_FLAG_HIDDEN);
    }
    // The caption shares its place with the setup hint.
    const bool caption = s_caption_on && !g_muse.setup_active;
    if (caption == lv_obj_has_flag(s_caption, LV_OBJ_FLAG_HIDDEN)) {
        if (caption) {
            lv_obj_remove_flag(s_caption, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_caption, LV_OBJ_FLAG_HIDDEN);
        }
    }
    face_apply();
}

void face_show_sign(const char *text, int ms)
{
    if (!s_sign || !text || !bsp_display_lock(500)) {
        return;
    }
    lv_label_set_text(s_sign, text);
    if (text[0] && ms > 0) {
        lv_obj_remove_flag(s_sign, LV_OBJ_FLAG_HIDDEN);
        s_sign_until_ms = muse_now_ms() + ms;
    } else {
        lv_obj_add_flag(s_sign, LV_OBJ_FLAG_HIDDEN);
        s_sign_until_ms = 0;
    }
    bsp_display_unlock();
}

void face_set_caption(const char *text)
{
    if (!text) {
        text = "";
    }
    if (strncmp(text, s_caption_text, sizeof(s_caption_text) - 1) == 0 && s_caption) {
        return;
    }
    if (!s_caption || !bsp_display_lock(500)) {
        return;  // try again next time
    }
    strlcpy(s_caption_text, text, sizeof(s_caption_text));
    lv_label_set_text(s_caption, s_caption_text);
    s_caption_on = s_caption_text[0] != '\0';
    bsp_display_unlock();
}

void face_create(lv_obj_t *parent)
{
    for (int i = 0; i < 2; i++) {
        part_init(&s_eye[i], make_blob(parent, COL_EYE));
        part_init(&s_highlight[i], make_blob(parent, COL_HIGHLIGHT));
        part_init(&s_happy[i], make_arc(parent, COL_LINE, true, 205, 335));   // ^ ^
        part_init(&s_asleep[i], make_arc(parent, COL_LINE, true, 25, 155));   // closed, curving down
        part_init(&s_dizzy[i], make_arc(parent, COL_EYE, true, 0, 290));
        part_init(&s_squint[i], make_line(parent, COL_LINE, s_squint_pts[i]));
    }
    part_init(&s_smile, make_arc(parent, COL_LINE, true, 35, 145));
    part_init(&s_open_mouth, make_arc(parent, COL_MOUTH, false, 0, 180));
    part_init(&s_tongue, make_blob(parent, COL_TONGUE));
    part_init(&s_round_mouth, make_blob(parent, COL_MOUTH));

    for (int i = 0; i < 5; i++) {
        part_init(&s_cloud[i], make_blob(parent, COL_CLOUD));
    }
    for (int i = 0; i < 3; i++) {
        part_init(&s_cloud_dot[i], make_blob(parent, COL_CLOUD_DOT));
    }

    // A sign the AI can put up (muse_show), at the top of the screen.
    s_sign = lv_label_create(parent);
    lv_label_set_text(s_sign, "");
    lv_obj_set_size(s_sign, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_sign, 300, 0);
    lv_obj_set_style_text_font(s_sign, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_sign, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_sign, lv_color_hex(0x2A211C), 0);
    lv_obj_set_style_bg_color(s_sign, lv_color_hex(COL_CLOUD), 0);
    lv_obj_set_style_bg_opa(s_sign, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_sign, 18, 0);
    lv_obj_set_style_pad_hor(s_sign, 16, 0);
    lv_obj_set_style_pad_ver(s_sign, 6, 0);
    lv_obj_align(s_sign, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_remove_flag(s_sign, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_sign, LV_OBJ_FLAG_HIDDEN);

    s_zzz = lv_label_create(parent);
    lv_label_set_text(s_zzz, "");
    lv_obj_set_style_text_font(s_zzz, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_zzz, lv_color_hex(COL_ZZZ), 0);
    lv_obj_set_style_text_opa(s_zzz, LV_OPA_70, 0);
    lv_obj_remove_flag(s_zzz, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_zzz, LV_OBJ_FLAG_HIDDEN);

    // Shown only while the setup portal is open.
    s_setup_hint = lv_label_create(parent);
    lv_label_set_text(s_setup_hint, "Setup: join Wi-Fi \"" WIFI_SETUP_SSID "\"");
    lv_obj_set_style_text_color(s_setup_hint, lv_color_white(), 0);
    lv_obj_set_style_text_opa(s_setup_hint, LV_OPA_80, 0);
    lv_obj_align(s_setup_hint, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_remove_flag(s_setup_hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_setup_hint, LV_OBJ_FLAG_HIDDEN);

    // The caption: what Claude Code is doing, and subtitles while Muse talks.
    // A dark pill keeps it readable when a close-up puts the body behind it;
    // on the wide shot it sits in the dark band under the body.
    s_caption = lv_label_create(parent);
    lv_label_set_text(s_caption, "");
    lv_obj_set_size(s_caption, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_caption, 332, 0);
    lv_obj_set_style_text_font(s_caption, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_caption, lv_color_hex(COL_CAPTION), 0);
    lv_obj_set_style_bg_color(s_caption, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_caption, LV_OPA_60, 0);
    lv_obj_set_style_radius(s_caption, 16, 0);
    lv_obj_set_style_pad_hor(s_caption, 14, 0);
    lv_obj_set_style_pad_ver(s_caption, 5, 0);
    lv_obj_align(s_caption, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_remove_flag(s_caption, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_caption, LV_OBJ_FLAG_HIDDEN);

    s_next_blink_ms = muse_now_ms() + 2500;
    s_next_glance_ms = muse_now_ms() + 1500;
    face_apply();
    lv_timer_create(face_tick, FACE_TICK_MS, NULL);
}
