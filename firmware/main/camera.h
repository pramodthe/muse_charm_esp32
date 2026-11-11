// SPDX-License-Identifier: MIT
// The camera: which part of the body frame is on screen, and how big.
//
// The body frames are rendered as one wide shot. The camera crops and enlarges
// them on the board (zoom.c), so Muse can come closer: a close-up while it
// talks, a peek right up to the glass, a medium shot for gestures. The face is
// drawn live on top (face.c), so it stays sharp at any zoom.
//
// Behaviour picks a shot; the camera eases to it over the next frames. Each
// frame that goes on screen publishes the view it was drawn with, and the face
// and the touch code use that, so everything stays locked to the body.
#pragma once

#include <stdbool.h>

typedef struct {
    float zoom;    // 1 shows the whole frame
    float ox, oy;  // frame position at the screen's top-left: screen = (frame - o) * zoom
} muse_view_t;

typedef enum {
    MUSE_SHOT_WIDE,    // the whole body
    MUSE_SHOT_MEDIUM,  // a step closer, arms and feet still in view (gestures, petting)
    MUSE_SHOT_CLOSE,   // head and shoulders (talking)
    MUSE_SHOT_PEEK,    // right up to the glass
} muse_shot_t;

// Frame size, and where the face rests in it (the close shots frame that point).
void camera_init(int width, int height, float face_x, float face_y);

// For the behaviour task --------------------------------------------------
// Ease to a shot over ease_ms. Does nothing if it is already the target.
void camera_shot(muse_shot_t shot, int ease_ms);

// Push in a little more on top of the shot, for example with your voice
// (0 = not at all, 0.05 = 5 % closer).
void camera_lean(float amount);

// The view for the frame about to be drawn. Returns false for the plain wide
// view, which needs no scaling.
bool camera_step(muse_view_t *view);

// True if the view has moved since the frame on screen was drawn.
bool camera_changed(void);

// With the display lock held ------------------------------------------------
// Record the view of the frame going on screen.
void camera_publish(const muse_view_t *view);

// The view on screen.
void camera_view(muse_view_t *view);

// Screen point to frame point, through the view on screen.
void camera_to_frame(int sx, int sy, float *fx, float *fy);
