// SPDX-License-Identifier: MIT
// Voice chat, relayed by the companion server on your computer. The server
// holds the Deepgram key and talks to the Deepgram Voice Agent API; Muse only
// streams audio over its companion connection.
//
//   hold BOOT        agent_begin_turn()  microphone streams to the server
//   let go           agent_end_turn()    Muse thinks, then the reply plays
//
// Progress is published in g_muse.agent_state. The AI also has Muse's body
// and screen as tools: the moves it asks for queue up here for the behaviour
// task (agent_next_move), a sign goes straight to the screen, and brightness
// and volume are applied as they arrive.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Allocate buffers and start the agent task. Call once at start-up.
void agent_init(void);

// True when linked to the companion server and it has a Deepgram key.
bool agent_configured(void);

// Start listening. Returns false when voice chat is not available.
bool agent_begin_turn(void);

// Finish the utterance; the reply follows.
void agent_end_turn(void);

// Stop the reply that is playing.
void agent_stop_speaking(void);

// For the audio task: whether to stream the microphone, the microphone
// samples, and reply audio to play (returns the number of samples copied).
bool agent_listening(void);
void agent_push_mic(const int16_t *pcm, int samples);
int agent_pull_speech(int16_t *pcm, int samples);

// For the behaviour task: the next move the AI asked for (a muse_move_t), or
// MUSE_MOVE_NONE.
int agent_next_move(void);
bool agent_move_waiting(void);

// For the behaviour task: stop waiting for a spoken reply (the AI answered
// with moves only).
void agent_done_waiting(void);

// For the behaviour task: the caption to show under Muse during a
// conversation ("Listening...", what it heard, then the reply as subtitles).
void agent_caption(char *buf, size_t len);

// For companion.c: the link and what the server says.
void agent_on_link_lost(void);
void agent_on_voice_ready(bool on);
struct cJSON;
void agent_on_voice(const struct cJSON *msg);  // a {"type":"voice", ...} message
void agent_on_audio(const void *data, int len);
