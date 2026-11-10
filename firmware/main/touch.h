// SPDX-License-Identifier: MIT
// Touch gestures: drag, tap, belly tap, long press and swipe.
#pragma once

#include "lvgl.h"

// Listen for touches on obj (the full-screen canvas).
void touch_attach(lv_obj_t *obj);
