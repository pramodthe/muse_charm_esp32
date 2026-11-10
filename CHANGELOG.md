# Changelog

## 0.4.0 (unreleased)

- Voice chat moved to the companion server: Muse streams audio to your computer, which holds the Deepgram key and talks to the Deepgram Voice Agent API. No key to type on Muse; the on-screen key page and the voice fields on the phone page are gone, and keys saved by 0.3.0 are erased from the board. Default LLM is Claude Haiku 4.5 through Deepgram; voice, model and personality are set in `~/.config/muse/voice.json`. Ask Muse what Claude is doing.
- Claude Code companion: Muse shows what Claude Code is doing on your computer (thinking, running a tool, waiting for your permission, done), with a caption under Muse. `companion/muse_companion.py` receives Claude Code's hooks and `companion/install_hooks.py` adds them. Muse finds the server with a UDP beacon.

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
