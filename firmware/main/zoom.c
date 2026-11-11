// SPDX-License-Identifier: MIT
// Bilinear zoom for RGB565 frames.
//
// Two passes with 5-bit weights. Each source row is stretched horizontally
// once into a line buffer (kept in "spread" form, see below), and every output
// row blends the two line buffers around it. Zooming in, a source row serves
// about `zoom` output rows, so this does far fewer blends than a per-pixel
// four-tap filter.
//
// Spread form: an RGB565 pixel with its green field moved up 16 bits
// (0x07E0F81F), which leaves room to multiply all three channels by a 5-bit
// weight in one 32-bit multiplication.

#include "zoom.h"

#define MASK 0x07E0F81Fu

static uint16_t s_xi[ZOOM_MAX_WIDTH];      // left source column for each output column
static uint8_t s_xw[ZOOM_MAX_WIDTH];       // weight of the column to its right, 0..32
static uint32_t s_line[2][ZOOM_MAX_WIDTH];  // two stretched source rows, by row parity

static inline uint32_t spread(uint16_t c)
{
    return (c | ((uint32_t)c << 16)) & MASK;
}

static void stretch_row(const uint16_t *row, uint32_t *out, int w)
{
    for (int x = 0; x < w; x++) {
        const uint32_t a = spread(row[s_xi[x]]), b = spread(row[s_xi[x] + 1]);
        const uint32_t wt = s_xw[x];
        out[x] = ((a * (32 - wt) + b * wt) >> 5) & MASK;
    }
}

// Source position (16.16) of output pixel 0's centre, and the row or column it
// falls in with the weight of the next one.
static inline void locate(int32_t pos, int size, int *index, uint32_t *weight)
{
    if (pos < 0) {
        pos = 0;
    }
    int i = pos >> 16;
    uint32_t wt = ((uint32_t)pos >> 11) & 31;
    if (i >= size - 1) {
        i = size - 2;
        wt = 32;
    }
    *index = i;
    *weight = wt;
}

void zoom_rgb565(const uint16_t *src, uint16_t *dst, int w, int h, float zoom, float ox, float oy)
{
    if (w > ZOOM_MAX_WIDTH || w < 2 || h < 2 || zoom < 1.0f) {
        return;
    }
    const int32_t step = (int32_t)(65536.0f / zoom + 0.5f);
    int32_t u = (int32_t)((ox + 0.5f / zoom - 0.5f) * 65536.0f);
    for (int x = 0; x < w; x++, u += step) {
        int i;
        uint32_t wt;
        locate(u, w, &i, &wt);
        s_xi[x] = (uint16_t)i;
        s_xw[x] = (uint8_t)wt;
    }

    int have[2] = {-1, -1};
    int32_t v = (int32_t)((oy + 0.5f / zoom - 0.5f) * 65536.0f);
    for (int y = 0; y < h; y++, v += step, dst += w) {
        int r;
        uint32_t wt;
        locate(v, h, &r, &wt);
        for (int k = r; k <= r + 1; k++) {
            if (have[k & 1] != k) {
                stretch_row(src + (uint32_t)k * w, s_line[k & 1], w);
                have[k & 1] = k;
            }
        }
        const uint32_t *a = s_line[r & 1], *b = s_line[(r + 1) & 1];
        const uint32_t wa = 32 - wt;
        for (int x = 0; x < w; x++) {
            const uint32_t s = ((a[x] * wa + b[x] * wt) >> 5) & MASK;
            dst[x] = (uint16_t)(s | (s >> 16));
        }
    }
}
