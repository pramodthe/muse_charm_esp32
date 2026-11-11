// SPDX-License-Identifier: MIT
// Muse's chirps, synthesized at run time (no audio files).
#pragma once

#include <stdint.h>

typedef enum {
    MUSE_SOUND_HELLO,    // rising "hi!"
    MUSE_SOUND_GIGGLE,   // "he-he-he"
    MUSE_SOUND_SQUEEZE,  // "eee"
    MUSE_SOUND_CHEER,    // "yay!"
    MUSE_SOUND_DIZZY,    // falling "wooo"
    MUSE_SOUND_WHOOSH,   // "oh!"
    MUSE_SOUND_MMHM,     // "mm-hm"
    MUSE_SOUND_LISTEN,   // "mm?"  (voice chat: listening)
    MUSE_SOUND_THINK,    // "hmm"  (voice chat: thinking)
    MUSE_SOUND_COUNT,
} muse_sound_t;

// One note: pitch glides from f0 to f1 (Hz) over ms milliseconds.
// amp 0 is a rest. A note with ms == 0 ends the sound.
typedef struct {
    uint16_t f0, f1, ms;
    uint8_t amp;        // 0..100
    uint8_t vib_hz;     // vibrato rate
    uint8_t vib_depth;  // vibrato depth (Hz)
} muse_note_t;

extern const muse_note_t *const muse_sounds[MUSE_SOUND_COUNT];
