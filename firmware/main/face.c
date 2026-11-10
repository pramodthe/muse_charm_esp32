// SPDX-License-Identifier: MIT
// Live face: eyes and mouth drawn with LVGL on top of the body frames.
//
// The body frames are rendered without a face. Each frame carries anchors
// (eye and mouth centres, face roll and a suggested expression) exported from
// Blender, and this module draws the face there. Because the face is live it
// can follow a finger, blink and change expression at 30 fps while the body
// plays at 10 fps.

#include "face.h"

#include <math.h>
#include <stdint.h>

#include "clips.h"
#include "esp_random.h"
#include "muse.h"

#define GAZE_X_PX 3.5f
#define GAZE_Y_PX 2.5f
#define FACE_TICK_MS 33

#define COL_EYE 0x3a2f29
#define COL_HIGHLIGHT 0xfff6ee
#define COL_LINE 0x4a3830
#define COL_MOUTH 0x5a2a28
#define COL_TONGUE 0xe07a80

// One face object, with its last layout cached so unchanged parts are not
// invalidated (and redrawn) every tick.
typedef struct {
    lv_obj_t *obj;
    int x, y, w, h, rot;
    bool shown;
} part_t;

static part_t s_eye[2], s_highlight[2], s_happy[2], s_squint[2], s_dizzy[2];
static part_t s_smile, s_open_mouth, s_tongue, s_round_mouth;
static lv_point_precise_t s_squint_pts[2][3];

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

// Show a part centred on (cx, cy).
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

static void part_init(part_t *p, lv_obj_t *obj)
{
    p->obj = obj;
    p->x = p->y = p->w = p->h = p->rot = INT32_MIN;
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
static lv_obj_t *make_arc(lv_obj_t *parent, uint32_t color, int width, bool rounded, int start, int end)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
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
    lv_obj_set_style_line_width(l, 3, 0);
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

void face_apply(void)
{
    const muse_frames_header_t *hdr = clips_header();
    const muse_anchor_t *a = clips_anchor(g_muse.frame);
    const float ex[2] = {a->eye_l_x * 0.25f, a->eye_r_x * 0.25f};
    const float ey[2] = {a->eye_l_y * 0.25f, a->eye_r_y * 0.25f};
    const float mx = a->mouth_x * 0.25f, my = a->mouth_y * 0.25f;
    const float roll_deg = a->roll10 * 0.1f;
    const float roll = roll_deg * (float)M_PI / 180.0f;
    const int roll_i = (int)lroundf(roll_deg);
    const int expr = g_muse.expr_override >= 0 ? g_muse.expr_override : a->expr;
    const float gx = s_gx * GAZE_X_PX, gy = s_gy * GAZE_Y_PX;
    float ox, oy;

    // Eyes
    for (int i = 0; i < 2; i++) {
        if (expr == MUSE_EXPR_HAPPY) {
            roll_offset(roll, 0, 4.0f, &ox, &oy);
            part_show(&s_happy[i], ex[i] + ox, ey[i] + oy, 13, 13);
            arc_rotate(&s_happy[i], roll_i);
        } else {
            part_hide(&s_happy[i]);
        }

        if (expr == MUSE_EXPR_SQUEEZE) {
            part_show(&s_squint[i], ex[i], ey[i], 10, 13);
        } else {
            part_hide(&s_squint[i]);
        }

        if (expr == MUSE_EXPR_DIZZY) {
            part_show(&s_dizzy[i], ex[i], ey[i], 12, 12);
            arc_rotate(&s_dizzy[i], i == 0 ? s_spin : 360 - s_spin);
        } else {
            part_hide(&s_dizzy[i]);
        }

        if (expr == MUSE_EXPR_NORMAL || expr == MUSE_EXPR_TALK || expr == MUSE_EXPR_SURPRISED) {
            static const int8_t blink_h[] = {13, 8, 2, 2, 8};
            int w = hdr->eye_w, h = hdr->eye_h;
            if (expr == MUSE_EXPR_SURPRISED) {
                w += 2;
                h += 2;
            }
            if (s_blink_tick >= 0) {
                h = blink_h[s_blink_tick] * h / 13;
                w += (h < 6) ? 2 : 0;
            }
            part_show(&s_eye[i], ex[i] + gx, ey[i] + gy, w, h);
            if (h >= 8) {
                part_show(&s_highlight[i], ex[i] + gx - w * 0.22f, ey[i] + gy - h * 0.25f, 3, 3);
            } else {
                part_hide(&s_highlight[i]);
            }
        } else {
            part_hide(&s_eye[i]);
            part_hide(&s_highlight[i]);
        }
    }

    // Mouth
    if (expr == MUSE_EXPR_NORMAL) {
        roll_offset(roll, 0, -6.0f, &ox, &oy);
        part_show(&s_smile, mx + ox, my + oy, 20, 20);
        arc_rotate(&s_smile, roll_i);
    } else {
        part_hide(&s_smile);
    }

    if (expr == MUSE_EXPR_HAPPY || expr == MUSE_EXPR_SQUEEZE) {
        roll_offset(roll, 0, -2.5f, &ox, &oy);
        part_show(&s_open_mouth, mx + ox, my + oy, 18, 18);
        arc_rotate(&s_open_mouth, roll_i);
        roll_offset(roll, 0, 3.5f, &ox, &oy);
        part_show(&s_tongue, mx + ox, my + oy, 8, 4);
    } else {
        part_hide(&s_open_mouth);
        part_hide(&s_tongue);
    }

    if (expr == MUSE_EXPR_TALK || expr == MUSE_EXPR_DIZZY || expr == MUSE_EXPR_SURPRISED) {
        int w = 7, h = 7;
        if (expr == MUSE_EXPR_TALK) {
            const float level = g_muse.mouth_level;
            w = 9 + (int)(5.0f * level);
            h = 3 + (int)(11.0f * level);
        } else if (expr == MUSE_EXPR_DIZZY) {
            w = 8 + (s_spin / 90) % 2 * 2;
            h = 6;
        }
        part_show(&s_round_mouth, mx, my + 1.0f, w, h);
    } else {
        part_hide(&s_round_mouth);
    }
}

// Gaze smoothing, blinks, idle glances and the dizzy spin. Runs in the LVGL
// task, so the lock is already held.
static void face_tick(lv_timer_t *t)
{
    (void)t;
    const int64_t now = muse_now_ms();
    float tx, ty;
    if (g_muse.touch_down) {
        // Look at the finger.
        const muse_anchor_t *a = clips_anchor(g_muse.frame);
        const float cx = (a->eye_l_x + a->eye_r_x) * 0.125f;
        const float cy = (a->eye_l_y + a->eye_r_y) * 0.125f;
        tx = muse_clampf((g_muse.touch_x - cx) / 90.0f, -1.0f, 1.0f);
        ty = muse_clampf((g_muse.touch_y - cy) / 110.0f, -1.0f, 1.0f);
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
    face_apply();
}

void face_create(lv_obj_t *parent)
{
    for (int i = 0; i < 2; i++) {
        part_init(&s_eye[i], make_blob(parent, COL_EYE));
        part_init(&s_highlight[i], make_blob(parent, COL_HIGHLIGHT));
        part_init(&s_happy[i], make_arc(parent, COL_LINE, 3, true, 205, 335));
        part_init(&s_dizzy[i], make_arc(parent, COL_EYE, 2, true, 0, 290));
        // ">" for the left eye and "<" for the right eye, in a 10 x 13 box
        const int outer = i == 0 ? 2 : 8, inner = i == 0 ? 8 : 2;
        s_squint_pts[i][0] = (lv_point_precise_t){outer, 2};
        s_squint_pts[i][1] = (lv_point_precise_t){inner, 6};
        s_squint_pts[i][2] = (lv_point_precise_t){outer, 11};
        part_init(&s_squint[i], make_line(parent, COL_LINE, s_squint_pts[i]));
    }
    part_init(&s_smile, make_arc(parent, COL_LINE, 3, true, 35, 145));
    part_init(&s_open_mouth, make_arc(parent, COL_MOUTH, 9, false, 0, 180));
    part_init(&s_tongue, make_blob(parent, COL_TONGUE));
    part_init(&s_round_mouth, make_blob(parent, COL_MOUTH));

    s_next_blink_ms = muse_now_ms() + 2500;
    s_next_glance_ms = muse_now_ms() + 1500;
    face_apply();
    lv_timer_create(face_tick, FACE_TICK_MS, NULL);
}
