# Muse charm for the ESP32-S3-Touch-AMOLED-1.8

An interactive 3D mascot for the [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8).
Hold its side button and talk to it: Muse listens, thinks and answers out loud in its own little
voice, swaying as it speaks with the words running underneath (through a small server on your computer and the
[Deepgram Voice Agent API](https://developers.deepgram.com/docs/voice-agent)). It also waves when you tap it, giggles when you tap its belly, follows your
finger with its eyes, leans against the tilt of the board and gets dizzy when shaken. As a
[Claude Code companion](companion/README.md) it sits by your keyboard and shows what Claude Code
is doing: thinking, editing a file, waiting for your OK, or done.

> **A fun, non-commercial learning project.** I built this to learn 3D modelling, animation and
> embedded graphics by recreating something I liked: Meta's Muse Charm mascot, on a small dev board.
> It is not affiliated with or endorsed by Meta, and it is not meant to compete with or harm Meta in
> any way. "Muse", "Muse Charm" and the character design belong to Meta. Nothing here may be sold or
> used commercially, and I will take it down if Meta asks.

<p align="center">
  <img src="docs/images/muse_demo.gif" width="300" alt="Muse idling, waving and giggling">
</p>

<p align="center">
  <img src="docs/images/expressions.png" alt="Expressions: idle, eyes following, wave, giggle, cheer, lean, dizzy, listening, squeeze, swipe">
</p>

## What it does

| Input | Muse |
| --- | --- |
| Hold the BOOT button and talk | Chirps "mm?", tilts its head and listens, hums while it thinks, then answers out loud with subtitles underneath. Press BOOT again to interrupt |
| Drag a finger | Eyes follow your finger |
| Tap | Waves and chirps "hi!" |
| Tap the belly | Giggles |
| Hold the screen | Squeezes up with `> <` eyes, cheers when you let go |
| Hold the screen for 5 s | Opens setup |
| Swipe left or right | Gets brushed aside and glances that way |
| Tilt the board | Stays upright by leaning against the tilt, eyes look downhill |
| Shake | Gets dizzy |
| Nothing | Breathes, blinks and glances around |

While it answers, the AI can make Muse wave, giggle, cheer or go dizzy. Without voice chat (no
companion server, or no Deepgram key on it), pressing BOOT squeezes Muse instead (holding it 3 s
opens setup), and Muse just looks up and moves its mouth when it hears a voice. The chirps are synthesized on the board.

## Voice chat

Muse streams its microphone to the companion server on your computer, which holds your Deepgram
key and talks to the [Deepgram Voice Agent API](https://developers.deepgram.com/docs/voice-agent):
speech to text, a reply from an LLM (Claude Haiku 4.5 by default) and an Aura voice. The spoken
reply streams back to Muse. Muse needs no key of its own, so there is nothing to type on the
small screen except your WiFi password.

1. Get an API key from the [Deepgram console](https://console.deepgram.com). Usage is billed to
   your Deepgram account.
2. Save it on your computer, where the server looks for it (this asks for the key and hides what
   you paste; zsh, the macOS default shell):

   ```sh
   mkdir -p ~/.config/muse
   read -rs 'k?Paste your Deepgram key: ' && print -rn -- "$k" > ~/.config/muse/deepgram_key && unset k
   chmod 600 ~/.config/muse/deepgram_key
   ```

   Or set `DEEPGRAM_API_KEY` in the server's environment.
3. Start the companion server (next section). Setup opens on Muse's screen on first start, when
   none of the saved WiFi networks is in range, or after holding the screen for 5 seconds: pick your
   WiFi and type its password on the watch-style keyboard.
4. Hold the **BOOT** button on the side, talk, and let go. Press it again while Muse talks to
   interrupt. The screen stays free for touching Muse.

Ask "what is Claude doing?" and Muse checks on Claude Code for you.

To change the voice, the model or Muse's personality, create `~/.config/muse/voice.json`, for
example `{"voice": "aura-2-luna-en", "think_provider": "open_ai", "think_model": "gpt-4o-mini"}`.
The keys are `voice`, `pitch`, `listen_model`, `think_provider`, `think_model` and `prompt`.

Muse speaks with the Deepgram voice raised a little in pitch, so it sounds like the same small
creature that chirps; set `"pitch": 1` in `voice.json` for the voice unchanged.

The microphone streams only while you hold BOOT. The Deepgram connection opens on the first
hold, stays open between turns and closes after two quiet minutes.

## Claude Code companion

Muse can show what Claude Code is doing on your computer: it glances up while Claude thinks, reads
"Editing face.c" while Claude works, waves for attention when Claude needs your permission and
cheers when the reply is done. A small Python server on your computer (standard library only)
receives Claude Code's hooks and passes a mood and a caption to Muse over your WiFi; Muse finds
it by itself. The same server relays voice chat.

```sh
python3 companion/muse_companion.py     # leave running (on macOS: /usr/bin/python3, see below)
python3 companion/install_hooks.py      # once: adds the hooks to ~/.claude/settings.json
```

See [companion/README.md](companion/README.md) for what Muse does for each event, privacy and
troubleshooting.

## Hardware

| | |
| --- | --- |
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.8, **V2** (CO5300 display, CST820 touch). Tested. |
| Original board | SH8601 display, FT3168 touch. Untested: set the board package to `1.1.4` in `firmware/main/idf_component.yml`. |
| Uses | 368 x 448 AMOLED, touch, QMI8658 motion sensor, ES8311 microphone and speaker, 8 MB PSRAM, 16 MB flash |

## Quick start

You need [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/index.html)
(tested with v5.5.5). The build downloads the board package, LVGL and the drivers automatically.

```bash
cd firmware
idf.py set-target esp32s3
idf.py -p <PORT> flash monitor
```

`<PORT>` is the board's USB port, for example `/dev/cu.usbmodem101` on macOS, `/dev/ttyACM0` on Linux
or `COM5` on Windows. Flashing replaces whatever firmware is on the board; Waveshare publishes the
factory firmware in its repository if you want it back.

## Settings

`idf.py menuconfig` → **Muse charm**:

| Option | Default | |
| --- | --- | --- |
| Display brightness | 80 % | |
| Play chirps on the speaker / volume | on / 65 % | |
| React to voice / microphone gain | on / 30 dB | |
| Voice threshold above room noise | 2.8x | Lower if Muse ignores you, raise if it reacts to background noise |
| Tilt and shake | on | |
| Invert the lean direction | off | Turn on if Muse leans the wrong way |
| Tilt needed to start leaning | 7° | |
| Shake threshold | 8.0 m/s² | |
| Claude Code companion / chirps | on / on | |
| Voice chat through the companion | on | Voice, model and personality are set on the computer |
| Companion server address | empty | Found automatically; set it on networks that block broadcasts |
| Log microphone and motion readings | off | Prints levels every few seconds, for tuning |

## How it works

```
 Blender (blender/muse.blend)
   │  blender/export_clips.py: poses the rig for every clip, hides the face,
   │  renders 368 x 448 frames and exports where the eyes and mouth are
   ▼
 assets/frames/f_000.jpg …   assets/anchors.json
   │  tools/pack_frames.py (runs during the firmware build)
   ▼
 muse_frames.bin, linked into the app (about 1.5 MB, 156 frames)
   │
   ▼
 ESP32-S3
   clips.c     decodes one JPEG per tick into PSRAM and swaps it onto an LVGL canvas (10 fps)
   face.c      draws the eyes and mouth live at each frame's anchors (30 fps)
   behavior.c  picks the clip: idle, wave, giggle, cheer, lean, dizzy, listen, squeeze
   touch.c / motion.c / audio.c   gestures, tilt and shake, voice and chirps
   companion.c link to the companion server: Claude Code moods, voice chat audio both ways
 companion/ on your computer
   muse_companion.py  Claude Code hooks in, moods out; muse_voice.py relays voice to Deepgram
```

The QSPI display can refresh a full frame about 10 times a second, so the body runs at 10 fps.
The face is small, so redrawing only the eyes and mouth runs at 30 fps. That is what lets the eyes
track a finger smoothly and blink, and it lets one set of body frames carry every expression.

Each clip frame carries the eye and mouth positions, the face roll and a suggested expression.
The firmware can override the expression, for example to open the mouth with your voice while
listening.

## Repository layout

```
firmware/            ESP-IDF project
  main/
    main.c           start-up
    clips.c/.h       frame table, JPEG decoding, canvas
    face.c/.h        live eyes and mouth
    behavior.c/.h    reactions and the idle loop
    touch.c/.h       drag, tap, belly tap, long press, swipe
    motion.c/.h      QMI8658 tilt and shake
    audio.c/.h       ES8311 microphone and speaker
    sounds.c/.h      the chirps
    agent.c/.h       voice chat, relayed by the companion server
    wifi.c/.h        saved networks and reconnects
    portal.c/.h      the Muse-setup page
    settings.c/.h    NVS start-up
    setup_ui.c/.h    on-screen WiFi setup
    companion.c/.h   Claude Code companion client
    Kconfig.projbuild  menuconfig options
companion/
  muse_companion.py  server for the Claude Code companion and voice chat
  muse_voice.py      voice chat relay to the Deepgram Voice Agent API
  muse_ws.py         small websocket implementation
  install_hooks.py   adds or removes the Claude Code hooks
assets/              exported frames and anchors (input to the build)
blender/
  muse.blend         character, rig, studio lights, device camera
  export_clips.py    rebuilds the clips and re-exports assets/
tools/
  pack_frames.py     assets → muse_frames.bin (run by the build)
  encode_frames.sh   PNG renders → baseline JPEG
  preview_face.py    preview clips with the face on your computer
docs/images/         README images
```

## Changing the character or the clips

The clip motions are plain Python functions in `blender/export_clips.py` (`idle`, `giggle`,
`cheer`, `lean`, …). Edit one, then from the repository root:

```bash
blender -b blender/muse.blend --python blender/export_clips.py -- --out assets
```

This rewrites `assets/anchors.json` and renders the frames into `build/render/`. If `ffmpeg` is
installed it also encodes them into `assets/frames/`; otherwise run `tools/encode_frames.sh`.
Check the result without flashing:

```bash
pip install pillow
python3 tools/preview_face.py            # every clip → build/preview/muse_all.gif
python3 tools/preview_face.py --sheet    # expression sheet
```

Then rebuild the firmware. To add a clip, add its function to `CLIPS` in the export script and
play it from `firmware/main/behavior.c` with `clips_find("name")`.

Frames must stay 368 x 448 baseline JPEGs, and `Eye L`, `Eye R` and `Mouth Smile` must exist in
the scene, because the anchors are taken from them.

## Troubleshooting

- **Muse shakes its head when you hold BOOT:** voice chat couldn't start. The server's
  output says why: no key or an unusable key file, or an error from Deepgram (for example an
  invalid key or voice name).
- **Muse answers before you finish:** Deepgram ends the turn at a long pause. Keep talking, or
  hold again to add more.
- **Muse leans the wrong way when tilted:** turn on *Invert the lean direction* in menuconfig.
- **Muse doesn't react to your voice, or reacts to noise:** turn on *Log microphone and motion
  readings* and compare the `mic rms` level while talking with the `room noise` level, then adjust
  the voice threshold or microphone gain.
- **`i2s_channel_disable … has not been enabled yet` in the log:** harmless. The codec driver
  disables the channels once before configuring them.
- **`Please check pull-up resistances` in the log:** harmless. It comes from the touch controller probe.

## Built with

[ESP-IDF](https://github.com/espressif/esp-idf),
[LVGL](https://github.com/lvgl/lvgl),
[esp_new_jpeg](https://components.espressif.com/components/espressif/esp_new_jpeg),
[esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev),
[Deepgram](https://deepgram.com) for speech and voice, the WiFi and setup page from
[esp32-agui](https://github.com/pramodthe/esp32-agui),
Waveshare's [board package](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_8)
and [QMI8658 driver](https://components.espressif.com/components/waveshare/qmi8658), and
[Blender](https://www.blender.org) for the character.
