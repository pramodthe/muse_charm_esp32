// SPDX-License-Identifier: MIT
// Touch gestures (drag, tap, belly tap, hold, swipe) and the BOOT button.
#pragma once

#include "lvgl.h"

// Listen for touches on obj (the full-screen canvas) and start polling the
// BOOT button. Call with the LVGL lock held.
void touch_attach(lv_obj_t *obj);
