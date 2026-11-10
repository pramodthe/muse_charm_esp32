// SPDX-License-Identifier: MIT
// Live face: eyes and mouth drawn with LVGL on top of the body frames.
#pragma once

#include "lvgl.h"

// Create the face objects and start the 30 Hz face timer.
// Call with the LVGL lock held.
void face_create(lv_obj_t *parent);

// Lay the face out for the frame on screen. Call with the LVGL lock held.
void face_apply(void);

// One line of text under Muse ("" hides it). Call from the behaviour task,
// without the LVGL lock; unchanged text costs nothing.
void face_set_caption(const char *text);
