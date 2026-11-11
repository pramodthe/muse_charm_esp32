// SPDX-License-Identifier: MIT
// Keeps the AMOLED panel showing. A dip in the supply can make the panel drop
// its settings: the screen goes black while Muse keeps running. Every few
// seconds a small task writes the panel's settings again (sleep out, display
// on, pixel format, brightness). On a panel that is fine this changes nothing
// on screen; on one that reset itself it brings the picture back, and the
// animation repaints it within a frame.
#pragma once

// Start the task. Call after bsp_display_start().
void panel_guard_start(void);

// Set the display brightness (percent). Safe from any task.
void panel_brightness(int percent);
