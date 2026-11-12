// SPDX-License-Identifier: MIT
// Claude Code companion: Muse shows what Claude Code is doing on your computer.
//
// companion/muse_companion.py runs on your computer and announces itself on
// the local network with a UDP beacon, or runs on a host on the internet, whose
// address Muse is told once ("pair"). Muse connects to it over a websocket and
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
    COMPANION_ALERT,    // a timer is up; the caption is its label
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

// Tell the server how Muse is doing (battery, WiFi, position, noise ...) soon;
// it also goes out once a minute. The AI reads it with its muse_status tool.
void companion_report_status(void);

// Tell the server that someone has just picked Muse up or woken it. The first
// time in a morning, Muse says good morning.
void companion_report_awake(void);

// For update.c: where to download firmware from (the online server Muse is
// linked to, over HTTPS) and the Authorization header that goes with it. False
// unless Muse is linked to an online server over TLS with a token.
bool companion_update_source(char *url, size_t url_len, char *auth, size_t auth_len);

// Send to the server (voice chat). Return false when not linked.
bool companion_send_text(const char *text);
bool companion_send_binary(const void *data, size_t len);
