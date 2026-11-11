// SPDX-License-Identifier: MIT
// Body clips: the packed frame table, JPEG decoding and the on-screen canvas.
//
// Frames are decoded into one of two PSRAM RGB565 buffers while LVGL shows
// the other, the same path the board's stock Video app uses. When the camera
// is zoomed in (camera.c), the frame is decoded into a third buffer and
// enlarged from there (zoom.c).

#include "clips.h"

#include <string.h>

#include "bsp/esp-bsp.h"
#include "camera.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "face.h"
#include "muse.h"
#include "zoom.h"

static const char *TAG = "clips";

extern const uint8_t muse_frames_start[] asm("_binary_muse_frames_bin_start");
extern const uint8_t muse_frames_end[] asm("_binary_muse_frames_bin_end");

static const muse_frames_header_t *s_hdr;
static const muse_clip_t *s_clips;
static const muse_anchor_t *s_anchors;
static const uint32_t *s_offsets;
static const uint8_t *s_jpeg_data;

static jpeg_dec_handle_t s_jpeg;
static uint8_t *s_buf[2];
static uint8_t *s_stage;       // the decoded frame, before the camera enlarges it
static int s_stage_frame = -1;  // which frame s_stage holds
static size_t s_buf_size;
static int s_front;
static lv_obj_t *s_canvas;

static bool decode(int index, uint8_t *out)
{
    jpeg_dec_io_t io = {
        .inbuf = (uint8_t *)(s_jpeg_data + s_offsets[index]),
        .inbuf_len = (int)(s_offsets[index + 1] - s_offsets[index]),
        .outbuf = NULL,
    };
    jpeg_dec_header_info_t info = {0};
    if (jpeg_dec_parse_header(s_jpeg, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "frame %d: bad JPEG header", index);
        return false;
    }
    int out_len = 0;
    if (info.width != s_hdr->width || info.height != s_hdr->height ||
            jpeg_dec_get_outbuf_len(s_jpeg, &out_len) != JPEG_ERR_OK || out_len > (int)s_buf_size) {
        ESP_LOGW(TAG, "frame %d: unexpected size %dx%d", index, info.width, info.height);
        return false;
    }
    io.outbuf = out;
    return jpeg_dec_process(s_jpeg, &io) == JPEG_ERR_OK;
}

bool clips_init(void)
{
    s_hdr = (const muse_frames_header_t *)muse_frames_start;
    if (memcmp(s_hdr->magic, "MUS2", 4) != 0 || s_hdr->nframes == 0 || s_hdr->nclips == 0) {
        ESP_LOGE(TAG, "muse_frames.bin is not a MUS2 clip table");
        return false;
    }
    s_clips = (const muse_clip_t *)(muse_frames_start + sizeof(muse_frames_header_t));
    s_anchors = (const muse_anchor_t *)(s_clips + s_hdr->nclips);
    s_offsets = (const uint32_t *)(s_anchors + s_hdr->nframes);
    s_jpeg_data = (const uint8_t *)(s_offsets + s_hdr->nframes + 1);
    ESP_LOGI(TAG, "%u frames in %u clips, %ux%u, %u bytes", s_hdr->nframes, s_hdr->nclips, s_hdr->width,
             s_hdr->height, (unsigned)(muse_frames_end - muse_frames_start));

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (jpeg_dec_open(&config, &s_jpeg) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG decoder open failed");
        return false;
    }

    s_buf_size = ((size_t)s_hdr->width * s_hdr->height * 2 + 15U) & ~(size_t)15U;
    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_aligned_calloc(16, 1, s_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_buf[i] == NULL) {
            ESP_LOGE(TAG, "no PSRAM for frame buffer %d", i);
            return false;
        }
    }
    s_stage = heap_caps_aligned_calloc(16, 1, s_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_stage == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the zoom buffer");
        return false;
    }

    // The close shots frame the face where it rests in the idle clip.
    const muse_clip_t *idle = clips_find("idle");
    float face_x = s_hdr->width * 0.5f, face_y = s_hdr->height * 0.3f;
    if (idle && idle->count > 0) {
        float sum_x = 0, sum_y = 0;
        for (int i = 0; i < idle->count; i++) {
            const muse_anchor_t *a = &s_anchors[idle->first + i];
            sum_x += (a->eye_l_x + a->eye_r_x) * 0.125f;
            sum_y += (a->eye_l_y + a->eye_r_y) * 0.125f;
        }
        face_x = sum_x / idle->count;
        face_y = sum_y / idle->count;
    }
    camera_init(s_hdr->width, s_hdr->height, face_x, face_y);

    s_front = 0;
    g_muse.frame = 0;
    return decode(0, s_buf[0]);
}

const muse_frames_header_t *clips_header(void)
{
    return s_hdr;
}

const muse_clip_t *clips_find(const char *name)
{
    for (int i = 0; i < s_hdr->nclips; i++) {
        if (strncmp(s_clips[i].name, name, sizeof(s_clips[i].name)) == 0) {
            return &s_clips[i];
        }
    }
    ESP_LOGE(TAG, "clip '%s' is missing from muse_frames.bin", name);
    return NULL;
}

const muse_anchor_t *clips_anchor(int frame)
{
    return &s_anchors[frame];
}

lv_obj_t *clips_create_canvas(lv_obj_t *parent)
{
    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_buf[s_front], s_hdr->width, s_hdr->height, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_canvas);
    return s_canvas;
}

void clips_show(int frame)
{
    const int back = s_front ? 0 : 1;
    muse_view_t view;
    if (!camera_step(&view)) {
        if (!decode(frame, s_buf[back])) {
            return;
        }
    } else {
        static bool timed;
        const int64_t t0 = esp_timer_get_time();
        if (s_stage_frame != frame) {
            s_stage_frame = -1;
            if (!decode(frame, s_stage)) {
                return;
            }
            s_stage_frame = frame;
        }
        const int64_t t1 = esp_timer_get_time();
        zoom_rgb565((const uint16_t *)s_stage, (uint16_t *)s_buf[back], s_hdr->width, s_hdr->height, view.zoom,
                    view.ox, view.oy);
        if (!timed) {
            timed = true;
            ESP_LOGI(TAG, "first zoomed frame: decode %d ms, zoom %d ms", (int)((t1 - t0) / 1000),
                     (int)((esp_timer_get_time() - t1) / 1000));
        }
    }
    if (!bsp_display_lock(2000)) {
        ESP_LOGW(TAG, "display busy for 2 s, frame %d skipped", frame);
    } else {
        lv_canvas_set_buffer(s_canvas, s_buf[back], s_hdr->width, s_hdr->height, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(s_canvas);
        g_muse.frame = frame;
        camera_publish(&view);
        face_apply();  // move the face with the body in the same refresh
        bsp_display_unlock();
    }
    s_front = back;
}

bool clips_refresh(void)
{
    if (!camera_changed()) {
        return false;
    }
    clips_show(g_muse.frame);
    return true;
}
