# Muse charm for the ESP32-S3-Touch-AMOLED-1.8

An interactive 3D mascot for the
[Waveshare ESP32-S3-Touch-AMOLED-1.8](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8).
Muse lives on the little AMOLED screen: it waves when you tap it, purrs when you stroke its head,
follows your finger with its eyes, leans against the tilt of the board and gets dizzy when shaken.
Left alone it fidgets, comes up to the glass for a look at you, and after a while dozes off.

Hold the side button and talk to it, and Muse comes in close and answers out loud in its own small
voice, swaying as it speaks with subtitles underneath. Next to your keyboard it doubles as a **Claude Code
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
| Hold the **BOOT** button and talk | Comes in close, chirps "mm?" and tilts its head, leaning in as you speak; hums while it thinks, then answers out loud in close-up with subtitles underneath. Press BOOT again to interrupt |
| Drag a finger | Eyes follow your finger |
| Stroke its head | Closes its eyes happily, purrs and leans its head into your finger |
| Tap | Steps closer, waves and chirps "hi!" |
| Tap the belly | Giggles |
| Tap the face | A startled "oh!", then a giggle, up close |
| Hold the screen | Squeezes up with `> <` eyes, cheers when you let go |
| Hold the screen for 5 s | Opens setup |
| Swipe left or right | Gets brushed aside and glances that way |
| Tilt the board | Stays upright by leaning against the tilt, eyes look downhill |
| Shake | Gets dizzy |
| Pick it up | Looks at you, pleased |
| Use Claude Code | Shows what Claude is doing, comes up to the glass and waves when it needs you, cheers when it's done |
| Nothing | Breathes, blinks and glances around; now and then peeks at you up close, looks around, wiggles or stretches |
| Nothing for 3 minutes | Dozes off: eyes closed, "z z Z", the screen dimmed. Touch it, pick it up or press BOOT to wake it |
| Ask for something bigger: "research the best budget 3D printers" | Says it's on it, works in the background for a few minutes, then tells you the answer in a sentence; the write-up is on a web page |
| Set a timer, run its battery down, pick it up in the morning | Speaks first: "Time's up! Tea.", "I'm getting sleepy, could you plug me in?", a good morning |

The AI has Muse's body and screen as tools, so you can just ask: "dance for me", "come closer",
"show the word hello", "turn the volume down", "good night". It can sense things too: "how's your
battery?", "what time is it?", "set a timer for five minutes", "remember that I like tea". Ask
"what is Claude doing?" and it checks on your Claude Code sessions.

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
- The same server can run [online](#without-your-computer-the-server-online) instead, so Muse
  works anywhere there is WiFi, with no computer switched on.

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

## Without your computer: the server online

The companion server can run on a host on the internet instead of on your computer. Muse then
talks to you anywhere it has WiFi, and Claude Code can still report to it. It is the same
program in "online mode": Muse and the Claude Code hooks must present a token (a long random
password), and Muse connects over TLS and checks the server's certificate. It is not faster than
a server on your computer: the wait for a reply is Deepgram's, not the network's.

With [Modal](https://modal.com), which bills by the second while Muse is connected and stops the
server a few minutes after Muse disconnects (check Modal's pricing for the current free
allowance):

1. Log in to Modal once: `pip install modal`, then `modal setup` (with uv: `uvx modal setup`).
2. Deploy, from the repository root:

   ```sh
   python3 companion/deploy_modal.py
   ```

   It makes the token, stores the token and the keys from your `.env` (Deepgram, and Anthropic
   if you have one for [background jobs](#background-jobs)) in Modal as the secret
   `muse-companion`, deploys `companion/modal_app.py` and saves the server's address and the
   token in `~/.config/muse/server.json` on your computer. Run it again after changing the
   server's code, your key or your voice options.
3. Try it without Muse: `python3 companion/test_voice.py --online "Hello Muse!"`.
4. Tell Muse. With Muse on the same WiFi as your computer:

   ```sh
   /usr/bin/python3 companion/muse_companion.py pair
   ```

   When it says Muse saved the online server, stop it (Ctrl-C). Muse keeps the address and token
   in its own storage. From then on it looks for a server on the local network for three seconds
   and, finding none, connects to the online one. `pair --forget` makes Muse forget it.
5. Point Claude Code at it: `python3 companion/install_hooks.py --online`. Online, the hooks run
   `companion/muse_hook.py`, which keeps only what Muse shows (the event, the project folder's
   name, the tool, a file name or Claude's one-line description of a command) and sends that in
   the background. Your prompts, file contents and commands never leave your computer.
   `python3 companion/muse_hook.py --print < event.json` shows exactly what would be sent.

Any other host that runs containers and passes websockets through over https works too
(DigitalOcean App Platform, Fly.io, Render, a VPS behind a proxy): build
[`companion/Dockerfile`](companion/Dockerfile), set `DEEPGRAM_API_KEY` and `MUSE_TOKEN` in the
host's settings, and save `{"url": "wss://your-host/device", "token": "..."}` in
`~/.config/muse/server.json` before pairing.

What changes online: your voice travels from Muse to that host and on to Deepgram, and the host
holds your Deepgram key, so use a host you trust. Anyone with the token can use the server, so
keep `server.json` to yourself. To switch back, start the local server; a Muse that is already
connected online notices it after a restart.

## Voice chat

Hold **BOOT** on the side of the board, talk, and let go; the screen stays free for touching Muse.

- Muse chirps "mm?" and tilts its head while it listens; the caption says "Listening…".
- When you let go it hums, and a thought cloud appears over its head while it thinks; the caption
  shows what it heard.
- The reply plays in Muse's own voice: the Deepgram voice raised a little in pitch, so it sounds
  like the same small creature that chirps. The mouth follows the words, the body sways on each
  syllable, gestures land a moment into the reply, and the reply runs underneath as subtitles
  timed to the audio.
- Press BOOT again while Muse talks to interrupt it.

### What the AI can do with Muse

The model behind the voice is an agent with tools. It decides when to call them, the companion
server carries them out (`TOOLS` in [`companion/muse_voice.py`](companion/muse_voice.py)) and the
firmware performs them (`perform()` in `firmware/main/behavior.c`):

| Tool | What it does | Try saying |
| --- | --- | --- |
| `muse_do` | Performs one or more moves, in order: `wave`, `giggle`, `cheer`, `dance`, `spin`, `shake_head`, `tilt_head`, `bounce`, `sway`, `smile`, `surprised`, `come_closer`, `step_back`, `sleep` | "Dance for me", "come closer", "good night" |
| `muse_show` | Puts a short sign at the top of the screen for a few seconds | "Show the word hello" |
| `muse_set` | Changes the screen brightness or speaker volume, until the next restart | "A bit quieter", "dim your screen" |
| `muse_status` | Reads Muse's own sensors: battery and charging, WiFi, whether it is upright or lying down, how noisy the room is, brightness, volume, time since it started, free memory | "How's your battery?", "is it noisy here?" |
| `time_now` | The time, weekday and date where you are | "What time is it?" |
| `timer` | Sets, lists or cancels timers; when one is up Muse says so, waves and shows its label | "Set a timer for five minutes for tea" |
| `remember`, `forget` | Keeps short notes about you between conversations, and drops them again | "Remember my name is Sam", "forget everything" |
| `claude_status` | Reads what your Claude Code sessions are doing | "What is Claude doing?" |
| `work_on`, `work_status` | Hands research to a background job that searches the web and writes up what it finds; says how jobs are going and what they found | "Research the best budget 3D printers", "what did you find?" |

Muse reports its sensors to the server at the start of each conversation and once a minute
(`power.c` reads the board's AXP2101 for the battery). The server side of the other tools is in
[`companion/muse_world.py`](companion/muse_world.py). Notes are kept in
`~/.config/muse/memory.json` (online: on the host's storage), and timers live in the server, so
they are lost if it restarts.

Muse also uses the moves by itself to show feelings while it talks. To add a move, add it to
`MOVES` in `muse_voice.py`, to `muse_move_t` in `muse.h` and the name table in `agent.c`, and
play it in `perform()`.

### Background jobs

A spoken reply has to come within a second or two, which is too short to look anything up. So
bigger requests are handed off:

```
you: "research the best budget 3D printers"
  └─ the voice AI calls work_on ──> muse_jobs.py starts muse_worker.py, its own process
Muse: "On it! I'll tell you when it's ready."          │  a Deep Agent: plans, searches the
        ... Muse goes back to its day ...               │  web, reads, writes it up (minutes)
Muse: "I'm done! The ... is the best pick because ..." <┘  the summary, spoken first
you:  read the write-up at /results
```

The worker is a [Deep Agent](https://github.com/langchain-ai/deepagents) running Claude with
Anthropic's web search. It keeps its notes in memory only, and cannot run commands or touch
files on the server. Each job is stopped after 12 minutes, two can run at once, and the last 30
are kept.

To set it up:

1. Create an [Anthropic API key](https://console.anthropic.com) and paste it into `.env`, after
   `ANTHROPIC_API_KEY=` (see [Setting up Deepgram](#setting-up-deepgram) for the file).

2. Online: run `python3 companion/deploy_modal.py` again, which hands the key to Modal with the
   others. On your computer: nothing more if [uv](https://docs.astral.sh/uv/) is installed (it
   fetches the worker's packages on the first job); otherwise
   `pip install deepagents langchain-anthropic` for a Python 3.11 or later and name it in
   `MUSE_WORKER_PYTHON`.
3. Open the web app: from the repository's folder, `python3 companion/muse_companion.py results`
   opens it in your browser, signed in (see [The web app](#the-web-app)).

A research job uses far more of the model than a spoken reply, and it is billed to your
Anthropic account. `worker_model` in `voice.json` chooses the model. The worker is told today's
date and what Muse remembers about you, so "near where I live" works once Muse knows where
that is.

### The web app

The server has a small web app at `/results`, for the things a tiny screen is bad at. It is
laid out as a chat with Muse, in the manner of [OpenMuse](https://github.com/CopilotKit/openmuse),
with colours and sizes from the Meta Horizon OS UI Set:

| | |
| --- | --- |
| Chat | Your requests on the right, Muse's answers on the left, and a card for each write-up that opens it to read. Type a new request at the bottom; it starts the same background job as asking out loud, and Muse announces the result |
| Ideas | Things to hand over (a tap puts one in the message box) and things to say out loud |
| Memory | What Muse remembers about you: add a note, or forget one |
| At the top | Whether Muse is connected, its battery, and running timers |

For your own picture of Muse at the top, save it as `companion/avatar.png` (and deploy again
if the server is online).

**Opening it, signed in.** Run this from the repository's folder (the path is wherever you
cloned it):

```sh
cd ~/path/to/muse_charm_esp32
python3 companion/muse_companion.py results
```

It opens the app in your default browser, already signed in. That browser stays signed in, so
bookmark the page; you do this once per browser. On a phone or another browser, open the same
address and paste the token when it asks. This copies the token to the clipboard on macOS
without showing it:

```sh
python3 -c "import json,os;print(json.load(open(os.path.expanduser('~/.config/muse/server.json')))['token'],end='')" | pbcopy
```

**Why there is a sign-in.** An online server is on the public internet. Typing a request starts
a research job that is billed to your Anthropic account, and Memory holds personal notes, so
both are only for whoever has the token (the one `deploy_modal.py` made, kept in
`~/.config/muse/server.json`). On your own computer, without an online server, there is no
token and no sign-in.

**Reading without signing in.** Add `MUSE_RESULTS_PUBLIC=true` to `.env` and deploy again:
anyone who has the server's address can then read your requests and write-ups. Asking, Memory
and Muse's status still need the token.

The page is one file, [`companion/muse_results.py`](companion/muse_results.py), with no build
step and nothing loaded from other sites.

### Muse speaks first

Muse does not only answer. It wakes up, comes close and says something by itself when:

| When | Muse |
| --- | --- |
| A timer is up | "Time's up! Tea.", with a wave and the label as a sign |
| A background job is finished | "I'm done!" and the answer in a sentence, with a cheer |
| Its battery falls to 20 %, and again at 10 % | Asks to be plugged in, with the level as a sign. Not between 10 pm and 8 am |
| You pick it up, wake it or switch it on for the first time in a morning (5 to 11 am) | Says good morning in its own words, and waves |

Hold BOOT to answer it. The server starts these (`Voice.announce()` in `muse_voice.py`, the
wording and limits in `muse_world.py`): a timer and the battery are spoken word for word, and for
the greeting the AI is handed a note and finds its own words, so it can use your name if it
remembers it. Muse plays them the way it plays any reply, so they need no extra memory on the
board. If Muse cannot speak (no Deepgram key, or `"speak_first": false` in `voice.json`), a timer
still ends with chirps, a wave and the sign.

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
3. Put it in the `.env` file in the repository's folder, on the computer that runs the
   companion server:

   ```sh
   cp .env.example .env && chmod 600 .env
   ```

   Open `.env` in an editor and paste the key after `DEEPGRAM_API_KEY=`. Or skip the file and
   set `DEEPGRAM_API_KEY` in the server's environment.
4. Check it: `curl http://127.0.0.1:8765/` shows `"voice": "ready"` within a few seconds (no
   restart needed), or a message saying what is wrong with the key. For a full round trip
   without Muse, run `python3 companion/test_voice.py "Hello Muse!"` and play `reply.wav`.

**Where the key lives.** Only in `.env` on your computer. The file is listed in `.gitignore`,
so git never sees it (`.env.example`, without keys, is the one in the repository). The key is
never sent to Muse or stored on the board; the server uses it to talk to Deepgram. To change
it, edit `.env`; to stop voice chat, empty the line; if you think it has leaked, revoke it in
the Deepgram console. A key saved by an earlier version in `~/.config/muse/deepgram_key` still
works while `.env` has none. Anyone on your local network
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
| `timezone` | the server's own | An IANA name such as `Australia/Sydney`, for the time tool |
| `speak_first` | `true` | `false`: Muse only speaks when spoken to (see [Muse speaks first](#muse-speaks-first)) |
| `worker_model` | `anthropic:claude-sonnet-5-5` | The model for [background jobs](#background-jobs), as Deep Agents names it |

To test voice chat without Muse, `python3 companion/test_voice.py "Hello Muse!"` plays Muse's part
from your computer (macOS `say`, or `--wav file.wav`) and saves the spoken reply to `reply.wav`.

## Claude Code companion

| Claude Code | Muse |
| --- | --- |
| A session starts | Waves, shows the project name |
| Thinking | A thought cloud overhead, "Thinking..." |
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
| Close-up zoom while Muse talks | 145 % | The body picture gets softer the closer you go; the face stays sharp |
| Fidget when nothing is happening | on | Silent |
| Doze off after | 180 s | 0 to never sleep |
| Display brightness while asleep | 20 % | |
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
   camera.c      which part of the frame is on screen, and how big (zoom.c enlarges it)
   face.c        draws the eyes, mouth and caption live at each frame's anchors (30 fps)
   behavior.c    picks the clip, the expression and the camera shot: idle, wave, giggle, cheer,
                 lean, dizzy, listen, squeeze, talking, petting, fidgets, sleep
   touch.c / motion.c / audio.c   gestures and BOOT, tilt and shake, microphone, speaker, chirps
   companion.c   the link to the companion server; agent.c streams voice chat over it
```

The QSPI display can refresh a full frame about 10 times a second, so the body runs at 10 fps.
The face is small, so redrawing only the eyes and mouth runs at 30 fps. That is what lets the eyes
track a finger smoothly and blink, and it lets one set of body frames carry every expression.
Each clip frame carries the eye and mouth positions, the face roll and a suggested expression,
which the firmware can override, for example to move the mouth with Muse's voice.

**The camera.** All the body frames are one wide shot. To come closer, `camera.c` picks a part of
the decoded frame and `zoom.c` enlarges it to the full screen with a two-pass bilinear filter
(about 27 ms a frame on the ESP32-S3, on top of 18 ms for the JPEG). Behaviour chooses a shot (wide,
medium for gestures, close for talking, a peek right at the glass) and the camera eases to it.
Because the face is drawn live, it is laid out through the same view at the new size, so in a
close-up the body is an enlarged picture but the eyes and mouth stay sharp. Touches are mapped
back through the view, so a tap on the belly is a tap on the belly at any zoom.

Voice chat streams 16 kHz audio both ways over the same websocket as the Claude Code moods. The
server sends the reply a couple of seconds ahead of playback, and each sentence's subtitle just
before its audio, so Muse can time the words. On this board, full-power WiFi can dip the supply
and blank the panel, so WiFi stays in modem sleep with larger TCP windows, transmit power is
capped at 14 dBm, and `panel.c` rewrites the panel's settings every few seconds.

## Repository layout

```
.env.example           your keys go in a copy of this, named .env (never committed)
firmware/              ESP-IDF project
  main/
    main.c             start-up
    clips.c/.h         frame table, JPEG decoding, canvas
    camera.c/.h        shots and easing: what part of the frame is on screen
    zoom.c/.h          bilinear zoom for RGB565 frames
    face.c/.h          live eyes, mouth and caption
    behavior.c/.h      reactions and the idle loop
    touch.c/.h         drag, taps, petting, long press, swipe, BOOT
    motion.c/.h        QMI8658 tilt, shake and pick-up
    audio.c/.h         ES8311 microphone and speaker
    sounds.c/.h        the chirps
    agent.c/.h         voice chat, relayed by the companion server
    companion.c/.h     link to the companion server, Claude Code moods
    wifi.c/.h          saved networks and reconnects
    setup_ui.c/.h      on-screen WiFi setup
    portal.c/.h        the Muse-setup page for phones
    panel.c/.h         keeps the AMOLED panel's settings, brightness
    power.c/.h         battery level and charging (AXP2101)
    settings.c/.h      NVS start-up
    Kconfig.projbuild  menuconfig options
companion/             runs on your computer
  muse_companion.py    the server: Claude Code hooks, moods, Muse's connection
  muse_voice.py        voice chat relay to the Deepgram Voice Agent API
  muse_world.py        the AI's time, timers, memory and view of Muse's sensors
  muse_jobs.py         background jobs: starts them, keeps their results
  muse_worker.py       one job: a Deep Agent that researches and writes up
  muse_results.py      the web app: write-ups, new requests, memory, Muse's status
  muse_ws.py           small websocket implementation
  muse_env.py          reads your keys from .env
  install_hooks.py     adds or removes the Claude Code hooks
  test_voice.py        tests voice chat without Muse
  deploy_modal.py      puts the server online with Modal
  modal_app.py         the server as a Modal app
  Dockerfile           the server for any other container host
  muse_hook.py         the Claude Code hook for an online server (sends only what Muse shows)
  muse_online.py       where the online server is (~/.config/muse/server.json)
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
python3 tools/preview_face.py --shots    # the camera's shots, as the board frames them
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
- **Muse never links to the online server.** Its serial log says why: a certificate or TLS error,
  "401" (pair again, the token changed), or no answer (the host is down). Check the server with
  `python3 companion/test_voice.py --online`.
- **Muse shakes its head when you hold BOOT:** voice chat couldn't start. The server's output says
  why: no key or an unusable key, or an error from Deepgram (for example an invalid voice
  name).
- **Muse says it can't do research yet:** the server has no Anthropic key. Put it in `.env`
  after `ANTHROPIC_API_KEY=` and, for an online server, run `python3 companion/deploy_modal.py`
  again.
- **A research job did not finish:** the web app says why under the request. A job is stopped
  after 12 minutes; other causes are an Anthropic account without credit or with web search
  turned off. `modal app logs muse-companion` shows the online server's log.
- **The web app asks you to sign in:** open it with `python3 companion/muse_companion.py results`
  from the repository's folder (see [The web app](#the-web-app)). "can't open file
  .../companion/muse_companion.py" means the Terminal is in another folder: `cd` to the
  repository first.
- **The web app looks like an older version after a deploy:** reload the page without the
  browser's cache (Cmd+Shift+R on macOS).
- **Muse answers before you finish:** Deepgram ends the turn at a long pause. Keep talking, or
  press BOOT again to add more.
- **The screen goes black but Muse still chirps:** the panel lost its settings in a supply dip.
  Muse writes them again every 3 seconds, which should bring the picture back; if it happens
  often, try another USB cable or port.
- **Muse stays on "Thinking..." after you interrupt Claude:** interrupted turns send no "done"
  event, so the mood clears by itself after five minutes, or at your next prompt.
- **Muse dozes off too soon, or you'd rather it never slept:** change *Doze off after* in
  menuconfig (0 turns sleep off). Typing on the same desk or moving the board keeps it awake.
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
