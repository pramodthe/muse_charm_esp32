// SPDX-License-Identifier: MIT
// Voice chat through the Deepgram Voice Agent API: one secure websocket that
// does speech-to-text, the AI reply and text-to-speech.
//
//   hold the screen  agent_begin_turn()  microphone streams to Deepgram
//   let go           agent_end_turn()    Muse thinks, then the reply plays
//
// Progress is published in g_muse.agent_state, and actions the AI asks for
// (wave, giggle, cheer, dizzy) in g_muse.agent_action.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Allocate buffers and start the agent task. Call once at start-up.
void agent_init(void);

// True when a Deepgram key is saved.
bool agent_configured(void);

// Re-check the key after setup (switches voice chat on or off).
void agent_refresh(void);

// Start listening (connects first if needed). Returns false if there is no
// key or no WiFi.
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
