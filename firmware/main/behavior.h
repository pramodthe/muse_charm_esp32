// SPDX-License-Identifier: MIT
// Behaviour: decides which clip plays and how the face looks.
#pragma once

#include <stdbool.h>

#include "muse.h"

// Find the clips and create the event queue. Call after clips_init().
bool behavior_init(void);

// Start the animation task (waves hello first).
void behavior_start(void);

// Queue a touch reaction. Safe to call from the LVGL task.
void behavior_post(muse_event_t event);
