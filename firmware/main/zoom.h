// SPDX-License-Identifier: MIT
// Bilinear zoom for RGB565 frames (plain C, no ESP-IDF dependencies).
#pragma once

#include <stdint.h>

#define ZOOM_MAX_WIDTH 480

// Fill dst (w x h) with the part of src (w x h) that starts at (ox, oy) and is
// 1/zoom of its size, so screen = (frame - origin) * zoom. zoom is 1 or more
// and the part must lie inside the frame. Not reentrant.
void zoom_rgb565(const uint16_t *src, uint16_t *dst, int w, int h, float zoom, float ox, float oy);
