# Changelog

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
