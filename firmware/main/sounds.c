// SPDX-License-Identifier: MIT
// Muse's chirps. Each is a short list of gliding notes; edit freely.
//   {start Hz, end Hz, milliseconds, amplitude 0..100, vibrato Hz, vibrato depth Hz}

#include "sounds.h"

#define REST(ms) {0, 0, (ms), 0, 0, 0}
#define END {0, 0, 0, 0, 0, 0}

static const muse_note_t HELLO[] = {
    {520, 760, 110, 90, 0, 0},
    REST(35),
    {680, 1150, 180, 100, 7, 25},
    END,
};

static const muse_note_t GIGGLE[] = {
    {1050, 880, 55, 80, 0, 0}, REST(45),
    {1120, 930, 55, 85, 0, 0}, REST(45),
    {1000, 840, 55, 80, 0, 0}, REST(45),
    {1160, 960, 60, 85, 0, 0}, REST(45),
    {1080, 860, 70, 75, 0, 0},
    END,
};

static const muse_note_t SQUEEZE[] = {
    {600, 1050, 300, 85, 10, 30},
    END,
};

static const muse_note_t CHEER[] = {
    {660, 680, 90, 85, 0, 0},
    {880, 900, 90, 90, 0, 0},
    {1320, 1400, 230, 100, 8, 30},
    END,
};

static const muse_note_t DIZZY[] = {
    {950, 350, 850, 90, 6, 90},
    END,
};

static const muse_note_t WHOOSH[] = {
    {720, 520, 130, 80, 0, 0},
    END,
};

static const muse_note_t MMHM[] = {
    {520, 540, 120, 70, 0, 0},
    REST(50),
    {620, 720, 160, 80, 0, 0},
    END,
};

static const muse_note_t LISTEN[] = {
    {560, 820, 140, 65, 0, 0},
    END,
};

static const muse_note_t THINK[] = {
    {470, 430, 280, 50, 5, 18},
    END,
};

const muse_note_t *const muse_sounds[MUSE_SOUND_COUNT] = {
    [MUSE_SOUND_HELLO] = HELLO,
    [MUSE_SOUND_GIGGLE] = GIGGLE,
    [MUSE_SOUND_SQUEEZE] = SQUEEZE,
    [MUSE_SOUND_CHEER] = CHEER,
    [MUSE_SOUND_DIZZY] = DIZZY,
    [MUSE_SOUND_WHOOSH] = WHOOSH,
    [MUSE_SOUND_MMHM] = MMHM,
    [MUSE_SOUND_LISTEN] = LISTEN,
    [MUSE_SOUND_THINK] = THINK,
};
