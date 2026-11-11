// SPDX-License-Identifier: MIT
// Keeps the AMOLED panel showing. A dip in the supply can make the panel drop
// its settings: the screen goes black while Muse keeps running. A small task
// reads the panel's power mode every two seconds and sets the panel up again
// when it is off. Pressing BOOT also sets it up again.
#pragma once

// Start watching. Call after bsp_display_start().
void panel_guard_start(void);

// Set the panel up again soon (safe from any task, including LVGL's).
void panel_wake(void);
