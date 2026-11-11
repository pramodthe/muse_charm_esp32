// SPDX-License-Identifier: MIT
// Claude Code companion: Muse shows what Claude Code is doing on your computer.
//
// companion/muse_companion.py runs on the computer and announces itself on the
// local network with a UDP beacon. Muse connects to it over a websocket and
// receives the current mood (thinking, working, needs you) with a short
// caption, plus one-off events (hello, done, oops...) to react to. Voice chat
// (agent.c) is relayed over the same connection.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define COMPANION_CAPTION_LEN 48

typedef enum {
    COMPANION_LINKED,   // connected to the computer
    COMPANION_HELLO,    // a Claude Code session started
    COMPANION_DONE,     // Claude finished its reply
    COMPANION_OOPS,     // a tool call failed
    COMPANION_ERROR,    // the turn failed
    COMPANION_COMPACT,  // Claude is tidying up its memory
    COMPANION_BYE,      // the session ended
} companion_event_kind_t;

typedef struct {
    companion_event_kind_t kind;
    char caption[COMPANION_CAPTION_LEN];
} companion_event_t;

// Start looking for the companion server (needs WiFi). Call once at start-up.
void companion_start(void);

bool companion_connected(void);

// For the behaviour task: the next one-off event, if any, and whether one is
// waiting.
bool companion_next_event(companion_event_t *out);
bool companion_event_waiting(void);

// The caption for the current mood (g_muse.claude_mood).
void companion_mood_caption(char *buf, size_t len);

// Send to the server (voice chat). Return false when not linked.
bool companion_send_text(const char *text);
bool companion_send_binary(const void *data, size_t len);
