// SPDX-License-Identifier: MIT
// The camera. See camera.h.

#include "camera.h"

#include <math.h>

#include "muse.h"

#ifdef CONFIG_MUSE_ZOOM_CLOSE_X100
#define CLOSE_ZOOM (CONFIG_MUSE_ZOOM_CLOSE_X100 / 100.0f)
#else
#define CLOSE_ZOOM 1.45f
#endif
#define MEDIUM_ZOOM 1.18f
#define PEEK_ZOOM (CLOSE_ZOOM * 1.25f)
#define CHEST_BELOW_FACE 85.0f  // frame pixels from the face down to the middle of the body

static float s_w = 368, s_h = 448, s_face_x = 184, s_face_y = 132;
static const muse_view_t WIDE = {.zoom = 1.0f};
static muse_view_t s_from = {.zoom = 1.0f}, s_to = {.zoom = 1.0f};  // behaviour task
static muse_view_t s_shown = {.zoom = 1.0f};                        // display lock
static muse_shot_t s_shot = MUSE_SHOT_WIDE;
static int64_t s_start_ms;
static int s_ease_ms;
static float s_lean;

// Keep the view inside the frame.
static muse_view_t inside(muse_view_t v)
{
    v.zoom = fmaxf(v.zoom, 1.0f);
    v.ox = muse_clampf(v.ox, 0.0f, s_w - s_w / v.zoom);
    v.oy = muse_clampf(v.oy, 0.0f, s_h - s_h / v.zoom);
    return v;
}

// A view at `zoom` that puts frame point (fx, fy) at (sx, sy) of the screen,
// given as fractions of its width and height.
static muse_view_t framing(float zoom, float fx, float fy, float sx, float sy)
{
    const muse_view_t v = {.zoom = zoom, .ox = fx - sx * s_w / zoom, .oy = fy - sy * s_h / zoom};
    return inside(v);
}

static muse_view_t shot_view(muse_shot_t shot)
{
    switch (shot) {
    case MUSE_SHOT_MEDIUM:
        return framing(MEDIUM_ZOOM, s_face_x, s_face_y + CHEST_BELOW_FACE, 0.5f, 0.52f);
    case MUSE_SHOT_CLOSE:
        return framing(CLOSE_ZOOM, s_face_x, s_face_y, 0.5f, 0.42f);
    case MUSE_SHOT_PEEK:
        return framing(PEEK_ZOOM, s_face_x, s_face_y + 4.0f, 0.5f, 0.45f);
    default:
        return WIDE;
    }
}

static muse_view_t current(void)
{
    float t = 1.0f;
    if (s_ease_ms > 0) {
        t = muse_clampf((float)(muse_now_ms() - s_start_ms) / (float)s_ease_ms, 0.0f, 1.0f);
    }
    const float e = t * t * (3.0f - 2.0f * t);  // ease in and out
    muse_view_t v = {
        .zoom = s_from.zoom + (s_to.zoom - s_from.zoom) * e,
        .ox = s_from.ox + (s_to.ox - s_from.ox) * e,
        .oy = s_from.oy + (s_to.oy - s_from.oy) * e,
    };
    if (s_lean > 0.001f) {
        // Zoom about the middle of the view.
        const float cx = v.ox + s_w / (2.0f * v.zoom), cy = v.oy + s_h / (2.0f * v.zoom);
        v.zoom *= 1.0f + s_lean;
        v.ox = cx - s_w / (2.0f * v.zoom);
        v.oy = cy - s_h / (2.0f * v.zoom);
    }
    return inside(v);
}

void camera_init(int width, int height, float face_x, float face_y)
{
    s_w = (float)width;
    s_h = (float)height;
    s_face_x = face_x;
    s_face_y = face_y;
}

void camera_shot(muse_shot_t shot, int ease_ms)
{
    if (shot == s_shot) {
        return;
    }
    s_lean = 0;
    s_from = current();
    s_to = shot_view(shot);
    s_shot = shot;
    s_start_ms = muse_now_ms();
    s_ease_ms = ease_ms;
}

void camera_lean(float amount)
{
    s_lean = muse_clampf(amount, 0.0f, 0.25f);
}

bool camera_step(muse_view_t *view)
{
    *view = current();
    if (view->zoom < 1.004f) {
        *view = WIDE;
        return false;
    }
    return true;
}

bool camera_changed(void)
{
    muse_view_t v;
    camera_step(&v);
    return fabsf(v.zoom - s_shown.zoom) > 0.004f || fabsf(v.ox - s_shown.ox) > 0.3f ||
           fabsf(v.oy - s_shown.oy) > 0.3f;
}

void camera_publish(const muse_view_t *view)
{
    s_shown = *view;
}

void camera_view(muse_view_t *view)
{
    *view = s_shown;
}

void camera_to_frame(int sx, int sy, float *fx, float *fy)
{
    *fx = s_shown.ox + (float)sx / s_shown.zoom;
    *fy = s_shown.oy + (float)sy / s_shown.zoom;
}
