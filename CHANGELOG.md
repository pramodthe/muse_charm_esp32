# Changelog

## 0.6.0 (2026-11-11)

- **The server can run online**, so Muse works anywhere there is WiFi with no computer on. Online mode (`MUSE_TOKEN` set) requires a token from Muse and from the hooks and turns the local beacon off. `deploy_modal.py` deploys it to Modal; a `Dockerfile` covers other hosts.
- Muse looks for a local server first and otherwise connects to the online one over TLS, checking its certificate. `muse_companion.py pair` tells Muse the address and token once; Muse keeps them in NVS.
- `install_hooks.py --online`: the hooks run `muse_hook.py`, which keeps only what Muse shows (no prompts, file contents or commands) and sends it in the background.
- `test_voice.py --online` and `muse_companion.py demo --online` test the deployed server.

## 0.5.0 (2026-11-11)

- **A camera.** The body frames are one wide shot; the board now crops and enlarges them itself (a two-pass bilinear zoom, about 27 ms a frame) and eases between shots: wide, medium, close and a peek right up to the glass. The face is drawn live through the same view, so it stays sharp at any zoom, and touches are mapped back through it. No new artwork.
- **Conversations in close-up.** Hold BOOT and Muse comes close and leans in as you speak; it answers in close-up, steps back for gestures and pulls back at the end. The caption sits on a dark pill so subtitles read over the body.
- **Petting.** Stroke Muse's head: happy closed eyes, a purr, and the head leans into your finger. A tap on the face is a boop; taps bring Muse a step closer.
- **Idle life.** Now and then Muse peeks at you up close, looks around, wiggles or stretches. Picked up after a rest, it looks at you, pleased.
- **Sleep.** After 3 quiet minutes Muse dozes off: eyes closed, "z z Z", slow breathing, the screen dimmed to 20 %. Touch, pick-up, BOOT or Claude Code wakes it. While Claude Code needs you, Muse comes right up to the glass before it waves.
- menuconfig: close-up zoom, fidgets on/off, doze-off time, sleep brightness. `tools/preview_face.py --shots` previews the camera's framing.
- Frames that run late no longer play fast afterwards to catch up.

## 0.4.0 (2026-11-11)

- **Companion server** (`companion/`, Python standard library only) on your computer. Muse finds it on the local network with a UDP beacon and keeps one websocket open to it.
- **Claude Code companion:** Muse shows what Claude Code is doing (thinking, running a tool, waiting for your permission, done) with a caption under Muse, waves when Claude needs you and cheers when it's done. `install_hooks.py` adds the hooks to `~/.claude/settings.json`.
- **Voice chat moved to the server:** Muse streams audio to your computer, which holds the Deepgram key and talks to the Deepgram Voice Agent API. Nothing to type on Muse but the WiFi password; keys saved on the board by 0.3.0 are erased. Claude Haiku 4.5 through Deepgram by default; voice, pitch, model and personality are set in `~/.config/muse/voice.json`. Ask Muse what Claude is doing.
- **Push to talk on the BOOT button**, so the screen stays free for touching Muse. Holding the screen squeezes Muse; holding it 5 s opens setup.
- **Voice and character feel like one:** Muse chirps "mm?" when it starts listening and hums while it thinks, speaks with the Deepgram voice raised in pitch, sways with each syllable, gestures a moment into the reply and settles with a happy look. The caption shows "Listening...", what it heard, and the reply as subtitles timed to the audio.
- `test_voice.py` tests voice chat without Muse; `muse_companion.py demo` plays a made-up Claude Code session.
- Steadier power: WiFi stays in modem sleep during voice chat (turning power saving off dipped the supply on this board) with bigger TCP windows, and the panel's settings are written again every few seconds so the screen recovers from a dip.
- A saved WiFi network that is in range but fails to join gets a second try before setup opens.
- MIT license for the code.

## 0.3.0 (2026-11-11)

- Voice chat through the Deepgram Voice Agent API: hold the screen to talk, Muse listens, thinks and answers out loud with its mouth following its voice. Holding again interrupts.
- The AI can make Muse wave, giggle, cheer or go dizzy while it talks (function calling).
- Muse-setup portal for WiFi, the Deepgram key, voice and personality, adapted from esp32-agui.
- Squeeze moved to the BOOT button; holding BOOT for 3 s opens setup.
- On-screen setup with a watch-style keyboard: pick WiFi, type the password, enter the Deepgram key on a hex keypad. Opens by itself when no saved network is in range. Tappable controls stay out of the rounded corners.
- WPA3 networks supported; enterprise networks are detected and explained.
- Fixed a dark screen: WiFi transmit power is capped at 14 dBm (full-power bursts dipped the supply), the brightness command is sent under the display lock, and the LVGL draw buffer lives in internal DMA memory.
- Logs a warning if the display task ever stalls.

## 0.2.0 (2026-11-10)

- Live face drawn with LVGL on top of the body frames, so the eyes follow your finger, blink and change expression at 30 fps.
- Reactions: wave, belly giggle, squeeze and cheer, swipe, tilt lean, shake dizzy, listening.
- Microphone voice detection and synthesized chirps on the ES8311 codec.
- QMI8658 tilt and shake. The sensor is soft-reset at start-up, so it works after a warm reboot from other firmware.
- menuconfig options for brightness, volume, voice, motion and debug logs.
- Frames and face anchors are packed during the build from `assets/`.
- `blender/export_clips.py` rebuilds every clip from the .blend file.

## 0.1.0 (2026-11-09)

- First version: pre-rendered wave and idle loop with blinks, and tap to wave.
