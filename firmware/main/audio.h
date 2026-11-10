// SPDX-License-Identifier: MIT
// ES8311 codec: microphone voice detection and the chirp synthesizer.
#pragma once

#include <stdbool.h>

#include "sounds.h"

// Open the codec and start the audio task. Returns false if the codec is
// missing; Muse then runs silently.
bool audio_start(void);

// Queue a chirp. Does nothing if sound is disabled or audio failed to start.
void audio_play(muse_sound_t sound);
