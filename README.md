# Muse charm for the ESP32-S3-Touch-AMOLED-1.8

An interactive 3D mascot for the
[Waveshare ESP32-S3-Touch-AMOLED-1.8](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8).
Muse lives on the little AMOLED screen: it waves when you tap it, giggles when you tap its belly,
follows your finger with its eyes, leans against the tilt of the board and gets dizzy when shaken.

Hold the side button and talk to it, and Muse answers out loud in its own small voice, swaying as
it speaks with subtitles underneath. Next to your keyboard it doubles as a **Claude Code
companion**: it glances up while Claude thinks, reads "Editing face.c" while Claude works, waves
when Claude needs your OK and cheers when the reply is done.

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

| You | Muse |
| --- | --- |
| Hold the **BOOT** button and talk | Chirps "mm?", tilts its head and listens, hums while it thinks, then answers out loud with subtitles underneath. Press BOOT again to interrupt |
| Drag a finger | Eyes follow your finger |
| Tap | Waves and chirps "hi!" |
| Tap the belly | Giggles |
| Hold the screen | Squeezes up with `> <` eyes, cheers when you let go |
| Hold the screen for 5 s | Opens setup |
| Swipe left or right | Gets brushed aside and glances that way |
| Tilt the board | Stays upright by leaning against the tilt, eyes look downhill |
| Shake | Gets dizzy |
| Use Claude Code | Shows what Claude is doing, waves when it needs you, cheers when it's done |
| Nothing | Breathes, blinks and glances around |

While Muse answers, the AI can make it wave, giggle, cheer or go dizzy. Ask "what is Claude
doing?" and it checks on your Claude Code sessions.

Without the companion server, Muse is still a full interactive mascot: pressing BOOT squeezes it
(holding 3 s opens setup), and it looks up and moves its mouth when it hears a voice. The chirps
are synthesized on the board.

## How the pieces fit

```
 Muse (the board)                companion server (your computer)         Deepgram Voice Agent API
 screen, touch, BOOT,  <──────>  companion/muse_companion.py  <───────>   speech to text,
 microphone, speaker     WiFi    holds the Deepgram key          wss      LLM reply, text to speech
                       websocket            ▲
                                            │ hooks, 127.0.0.1:8765
                                       Claude Code
```

- **The firmware** is the character: animation, touch, motion, chirps and on-screen WiFi setup.
- **The companion server** is a small Python program (standard library only) on your computer.
  It relays voice chat to Deepgram, so the API key never goes on the board, and turns Claude
  Code's hooks into moods and captions for Muse.
- Muse finds the server by itself: the server announces itself on your network with a UDP beacon.

## Hardware

| | |
| --- | --- |
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.8, **V2** (CO5300 display, CST820 touch). Tested. |
| Original board | SH8601 display, FT3168 touch. Untested: set the board package to `1.1.4` in `firmware/main/idf_component.yml`. |
| Uses | 368 x 448 AMOLED, touch, BOOT button, QMI8658 motion sensor, ES8311 microphone and speaker, 8 MB PSRAM, 16 MB flash |
| Computer | macOS or Linux with Python 3.8+ for the companion server, on the same WiFi as Muse |

## Getting started

### 1. Flash the firmware

You need [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/index.html)
(tested with v5.5.5). The build downloads the board package, LVGL and the drivers automatically.

```bash
cd firmware
idf.py set-target esp32s3
idf.py -p <PORT> flash monitor
```

`<PORT>` is the board's USB port, for example `/dev/cu.usbmodem101` on macOS, `/dev/ttyACM0` on
Linux or `COM5` on Windows. Flashing replaces whatever firmware is on the board; Waveshare
publishes the factory firmware in its repository if you want it back. Every push to `main` is
also built by GitHub Actions, which keeps the binaries as a downloadable artifact.

### 2. Put Muse on your WiFi

Setup opens on Muse's screen the first time, and whenever none of its saved networks is in range.
Pick your network and type the password on the watch-style keyboard. To open it later, hold the
screen for 5 seconds. **Set up from a phone instead**, at the bottom of the list, opens a
`Muse-setup` network with a setup page at `192.168.4.1`.

### 3. Start the companion server

```sh
/usr/bin/python3 companion/muse_companion.py     # macOS
python3 companion/muse_companion.py              # Linux
```

Leave it running. Muse giggles and shows "Linked to …" when it finds it. On macOS, use Apple's
`/usr/bin/python3`: the firewall lets it accept Muse's connection, while a Homebrew Python needs
approving in System Settings → Network → Firewall → Options again after every upgrade.

### 4. Add a Deepgram key for voice chat

Voice chat needs a free [Deepgram](https://deepgram.com) account and an API key; everything else
works without one. See [Setting up Deepgram](#setting-up-deepgram) below; it takes a few minutes.
Then hold BOOT, talk, and let go.

### 5. Connect Claude Code

```sh
python3 companion/install_hooks.py
```

This adds the hooks to `~/.claude/settings.json`, keeping your other settings and a backup.
Restart Claude Code sessions that are already open. `install_hooks.py --remove` takes them out.

## Voice chat

Hold **BOOT** on the side of the board, talk, and let go; the screen stays free for touching Muse.

- Muse chirps "mm?" and tilts its head while it listens; the caption says "Listening…".
- When you let go it hums, glances up while it thinks, and the caption shows what it heard.
- The reply plays in Muse's own voice: the Deepgram voice raised a little in pitch, so it sounds
  like the same small creature that chirps. The mouth follows the words, the body sways on each
  syllable, gestures land a moment into the reply, and the reply runs underneath as subtitles
  timed to the audio.
- Press BOOT again while Muse talks to interrupt it.

Speech to text, the reply and the voice all come from the
[Deepgram Voice Agent API](https://developers.deepgram.com/docs/voice-agent), with Claude Haiku
4.5 (through Deepgram) as the default LLM. The microphone streams only while you hold BOOT. The
connection to Deepgram opens on the first hold, stays open between turns and closes after two
quiet minutes.

### Setting up Deepgram

[Deepgram](https://deepgram.com) runs the whole conversation in the cloud: it turns your speech
into text, gets a reply from an LLM and speaks it. You only need an account and an API key; the
companion server does the rest.

1. Sign up at [console.deepgram.com](https://console.deepgram.com). New accounts get free credit
   (a $200 credit without a credit card at the time of writing), which is plenty for chatting
   with Muse. After that, usage is billed per minute of conversation; see Deepgram's pricing page.
2. In the console, open **API Keys** and create a key. Copy it straight away, as the console may
   not show it again.
3. Save it on the computer that runs the companion server. This asks for the key and hides what
   you paste (zsh, the macOS default shell):

   ```sh
   mkdir -p ~/.config/muse
   read -rs 'k?Paste your Deepgram key: ' && print -rn -- "$k" > ~/.config/muse/deepgram_key && unset k
   chmod 600 ~/.config/muse/deepgram_key
   ```

   In bash (most Linux systems), the second line is
   `read -rsp 'Paste your Deepgram key: ' k && printf %s "$k" > ~/.config/muse/deepgram_key && unset k`.
   Or skip the file and set `DEEPGRAM_API_KEY` in the server's environment.
4. Check it: `curl http://127.0.0.1:8765/` shows `"voice": "ready"` within a few seconds (no
   restart needed), or a message saying what is wrong with the key file. For a full round trip
   without Muse, run `python3 companion/test_voice.py "Hello Muse!"` and play `reply.wav`.

**Where the key lives.** Only in `~/.config/muse/deepgram_key` on your computer, outside the
repository, so git never sees it. It is never sent to Muse or stored on the board; the server
uses it to talk to Deepgram. To change it, run step 3 again; to stop voice chat, delete the
file; if you think it has leaked, revoke it in the Deepgram console. Anyone on your local network
could talk through the server and spend your Deepgram credit (they can't read the key), so run
it on networks you trust.

**Where the Deepgram code is.** All of it is in
[`companion/muse_voice.py`](companion/muse_voice.py), on your computer: it opens the Voice Agent
websocket, sends the settings (models, voice, Muse's personality and the functions it can call),
relays the audio both ways and passes subtitles and gestures to Muse. The firmware has no
Deepgram code: `firmware/main/agent.c` only streams the microphone to the server and plays what
comes back. No other key is needed; the default LLM (Claude Haiku 4.5) runs through Deepgram and
is billed with it.

### Voice options

To change the voice, the model or Muse's personality, create `~/.config/muse/voice.json`:

| Key | Default | |
| --- | --- | --- |
| `voice` | `aura-2-thalia-en` | Any Deepgram Aura voice |
| `pitch` | `1.18` | How much higher Muse speaks; `1` for the voice unchanged |
| `listen_model` | `nova-3` | Deepgram speech-to-text model |
| `think_provider` / `think_model` | `anthropic` / `claude-haiku-4-5` | Any Deepgram think provider and model, for example `open_ai` / `gpt-4o-mini` |
| `prompt` | a short, playful Muse | Muse's personality |

To test voice chat without Muse, `python3 companion/test_voice.py "Hello Muse!"` plays Muse's part
from your computer (macOS `say`, or `--wav file.wav`) and saves the spoken reply to `reply.wav`.

## Claude Code companion

| Claude Code | Muse |
| --- | --- |
| A session starts | Waves, shows the project name |
| Thinking | Glances up and aside, "Thinking..." |
| Running a tool | Reads the caption: "Editing face.c", "Run the tests" |
| Waiting for your permission | Waves for attention (up to three times): "OK to flash the firmware?" |
| A tool call fails | A quiet, puzzled look |
| Finished its reply | Cheers: "Done!" |
| Compacting its memory | Squeezes: "Tidying up memory" |
| The session ends | Waves goodbye |

With several sessions open, Muse shows the one that most needs you. Prompts, file contents and
commands stay on your computer: Muse only receives the mood, a short description such as "Editing
face.c", and the project folder name. `python3 companion/muse_companion.py demo` plays a made-up
session, and `curl http://127.0.0.1:8765/` shows what the server sees. See
[companion/README.md](companion/README.md) for the details.

## Settings

Firmware options are in `idf.py menuconfig` → **Muse charm**:

| Option | Default | |
| --- | --- | --- |
| Display brightness | 80 % | |
| Play chirps on the speaker / volume | on / 65 % | Volume applies to Muse's voice too |
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
   clips.c       decodes one JPEG per tick into PSRAM and swaps it onto an LVGL canvas (10 fps)
   face.c        draws the eyes, mouth and caption live at each frame's anchors (30 fps)
   behavior.c    picks the clip and the expression: idle, wave, giggle, cheer, lean, dizzy,
                 listen, squeeze, talking
   touch.c / motion.c / audio.c   gestures and BOOT, tilt and shake, microphone, speaker, chirps
   companion.c   the link to the companion server; agent.c streams voice chat over it
```

The QSPI display can refresh a full frame about 10 times a second, so the body runs at 10 fps.
The face is small, so redrawing only the eyes and mouth runs at 30 fps. That is what lets the eyes
track a finger smoothly and blink, and it lets one set of body frames carry every expression.
Each clip frame carries the eye and mouth positions, the face roll and a suggested expression,
which the firmware can override, for example to move the mouth with Muse's voice.

Voice chat streams 16 kHz audio both ways over the same websocket as the Claude Code moods. The
server sends the reply a couple of seconds ahead of playback, and each sentence's subtitle just
before its audio, so Muse can time the words. On this board, full-power WiFi can dip the supply
and blank the panel, so WiFi stays in modem sleep with larger TCP windows, transmit power is
capped at 14 dBm, and `panel.c` rewrites the panel's settings every few seconds.

## Repository layout

```
firmware/              ESP-IDF project
  main/
    main.c             start-up
    clips.c/.h         frame table, JPEG decoding, canvas
    face.c/.h          live eyes, mouth and caption
    behavior.c/.h      reactions and the idle loop
    touch.c/.h         drag, tap, belly tap, long press, swipe, BOOT
    motion.c/.h        QMI8658 tilt and shake
    audio.c/.h         ES8311 microphone and speaker
    sounds.c/.h        the chirps
    agent.c/.h         voice chat, relayed by the companion server
    companion.c/.h     link to the companion server, Claude Code moods
    wifi.c/.h          saved networks and reconnects
    setup_ui.c/.h      on-screen WiFi setup
    portal.c/.h        the Muse-setup page for phones
    panel.c/.h         keeps the AMOLED panel's settings
    settings.c/.h      NVS start-up
    Kconfig.projbuild  menuconfig options
companion/             runs on your computer
  muse_companion.py    the server: Claude Code hooks, moods, Muse's connection
  muse_voice.py        voice chat relay to the Deepgram Voice Agent API
  muse_ws.py           small websocket implementation
  install_hooks.py     adds or removes the Claude Code hooks
  test_voice.py        tests voice chat without Muse
assets/                exported frames and anchors (input to the build)
blender/
  muse.blend           character, rig, studio lights, device camera
  export_clips.py      rebuilds the clips and re-exports assets/
tools/
  pack_frames.py       assets → muse_frames.bin (run by the build)
  encode_frames.sh     PNG renders → baseline JPEG
  preview_face.py      preview clips with the face on your computer
docs/images/           README images
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

- **Muse never shows "Linked to …".** Check that the server is running and Muse is on the same
  WiFi. If the serial log shows "Error read response for Upgrade header", the computer's firewall
  is blocking Python (see step 3). If it never finds the server, the network blocks broadcasts
  (guest WiFi, many offices): set the server address in menuconfig, for example
  `ws://192.168.1.20:8765/device`.
- **Muse shakes its head when you hold BOOT:** voice chat couldn't start. The server's output says
  why: no key or an unusable key file, or an error from Deepgram (for example an invalid voice
  name).
- **Muse answers before you finish:** Deepgram ends the turn at a long pause. Keep talking, or
  press BOOT again to add more.
- **The screen goes black but Muse still chirps:** the panel lost its settings in a supply dip.
  Muse writes them again every 3 seconds, which should bring the picture back; if it happens
  often, try another USB cable or port.
- **Muse stays on "Thinking..." after you interrupt Claude:** interrupted turns send no "done"
  event, so the mood clears by itself after five minutes, or at your next prompt.
- **Muse leans the wrong way when tilted:** turn on *Invert the lean direction* in menuconfig.
- **Muse doesn't react to your voice, or reacts to noise:** turn on *Log microphone and motion
  readings* and compare the `mic rms` level while talking with the `room noise` level, then adjust
  the voice threshold or microphone gain.
- **`i2s_channel_disable … has not been enabled yet` or `Please check pull-up resistances` in
  the log:** harmless messages from the codec and touch drivers.

## Built with

[ESP-IDF](https://github.com/espressif/esp-idf),
[LVGL](https://github.com/lvgl/lvgl),
[esp_new_jpeg](https://components.espressif.com/components/espressif/esp_new_jpeg),
[esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev),
[esp_websocket_client](https://components.espressif.com/components/espressif/esp_websocket_client),
the [Deepgram Voice Agent API](https://developers.deepgram.com/docs/voice-agent) for voice,
[Claude Code hooks](https://code.claude.com/docs/en/hooks) for the companion, the WiFi setup from
[esp32-agui](https://github.com/pramodthe/esp32-agui),
Waveshare's [board package](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_8)
and [QMI8658 driver](https://components.espressif.com/components/waveshare/qmi8658), and
[Blender](https://www.blender.org) for the character.

## License

The source code is under the [MIT License](LICENSE). The Muse character and its design belong to
Meta: the character art and model in `assets/`, `blender/muse.blend` and `docs/images/` are not
covered by that license (see the note at the top). Changes are listed in
[CHANGELOG.md](CHANGELOG.md).
