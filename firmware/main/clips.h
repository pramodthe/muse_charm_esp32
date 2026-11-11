// SPDX-License-Identifier: MIT
// Body clips: the packed frame table, JPEG decoding and the on-screen canvas.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// Layout of muse_frames.bin, written by tools/pack_frames.py ("MUS2").
typedef struct __attribute__((packed)) {
    char magic[4];
    uint16_t width, height, nframes, nclips, belly_x, belly_y, eye_w, eye_h;
} muse_frames_header_t;

typedef struct __attribute__((packed)) {
    char name[8];
    uint16_t first, count;
    uint8_t fps, flags;
    uint16_t reserved;
} muse_clip_t;

typedef struct __attribute__((packed)) {
    int16_t eye_l_x, eye_l_y, eye_r_x, eye_r_y, mouth_x, mouth_y;  // pixels x4
    int16_t roll10;                                                // face roll, degrees x10
    uint8_t expr, reserved;
} muse_anchor_t;

// Load the frame table, open the decoder and decode the first frame.
bool clips_init(void);

const muse_frames_header_t *clips_header(void);
const muse_clip_t *clips_find(const char *name);
const muse_anchor_t *clips_anchor(int frame);

// Create the full-screen canvas showing the current frame.
lv_obj_t *clips_create_canvas(lv_obj_t *parent);

// Decode a frame and put it on screen through the camera, moving the face
// with it.
void clips_show(int frame);

// Draw the frame on screen again if the camera has moved (for holding a pose
// while the camera eases). Returns true if it did.
bool clips_refresh(void);
