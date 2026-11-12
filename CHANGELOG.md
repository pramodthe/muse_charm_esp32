# Changelog

## 0.12.0 (2026-11-12)

- **Firmware updates over WiFi.** `python3 companion/muse_companion.py update` sends a build to the online server; Muse downloads it when it has been left alone for 20 seconds, with "Updating..." on a still screen, and restarts into it. Only the online server can do this, over TLS and with the token.
- A new image is on trial until Muse has linked to a server with it; if it crashes first or cannot link within ten minutes, Muse goes back to the firmware from before and refuses that image from then on.
- The flash has two app slots for this (`partitions.csv`), so firmware from before needs one more `idf.py flash` over the cable. WiFi and the server's address are kept.
- `CLAUDE.md` tells Claude Code sessions how the project is laid out and what its rules are.
- In the web app, tap a message and "Remove from the chat" takes it out of the conversation (`POST /chat {"remove": [...]}`), so Muse no longer reads it back either.

## 0.11.0 (2026-11-12)

- **Reminders.** "Remind me at 5 pm to call mum", out loud or typed: for today, a later day, or repeating every day, on weekdays or every week (the AI's new `reminder` tool). When one is due Muse wakes, says it, waves and shows a sign, and the line goes into the chat. They are kept in `reminders.json`, so they survive a restart; one that came due while the server was off or Muse was not connected is said when it is back.
- **A morning briefing.** The first good morning of the day now includes today's reminders, any you missed, and the research Muse finished since the last one.
- The web app's Memory tab lists your reminders with a button to cancel each (`POST /reminders`).
- Speaking first is sturdier: an announcement is tried again if Deepgram cannot be reached or sends no audio, and then falls back to chirps and a sign (before, it could be lost without either). The server log says how much was said.
- The typed chat no longer "corrects" itself about things it did in earlier messages.

## 0.10.0 (2026-11-12)

- **Type to Muse.** The web app's message box is now a real chat: Muse answers in a few seconds (Claude through the Anthropic API, `chat_model`), with the same tools as its voice. A typed "dance for me" makes the Muse on your desk dance; a bigger request starts a background job.
- **One conversation.** What you say to the device and what you type are kept together (`chat.json`) and shown as one chat in the web app; the voice and the typed chat are each given the recent part of it.
- Firmware: moves that arrive outside a spoken turn are performed by themselves.
- The tools are carried out by one function for both (`run_tool()`), and `/feed.json` gives the web app everything in one request.

## 0.9.0 (2026-11-12)

- **A web app** at `/results`, replacing the results page, laid out as a chat with Muse. Chat shows each request with what Muse said and a card that opens the write-up, and takes new requests typed at the bottom; Ideas lists things to hand over and things to say out loud; Memory shows what Muse remembers and lets you add or forget notes; the top shows whether Muse is connected, its battery and timers. `companion/avatar.webp` (or `.gif`, `.png`, `.jpg`), if present, is Muse's picture and the tab's icon. The layout follows OpenMuse, and the colours, radii and type sizes the Meta Horizon OS UI Set; it is light only.
- New server addresses for it: `/app.json`, `/ask` and `/memory`. They always need the token online.
- The research worker is told today's date and what Muse remembers about you.

## 0.8.0 (2026-11-11)

- **Background jobs.** Ask Muse for something bigger ("research the best budget 3D printers") and it says it is on it, a worker searches the web and writes up what it finds, and a few minutes later Muse tells you the answer in a sentence. The AI's new tools are `work_on` and `work_status`.
- The worker (`muse_worker.py`) is a [Deep Agent](https://github.com/langchain-ai/deepagents) running Claude with Anthropic's web search, started by `muse_jobs.py` as its own process. It needs an Anthropic API key (`~/.config/muse/anthropic_key`); `deploy_modal.py` hands it to Modal with the others.
- **A results page** at `/results` on the server for reading the write-ups; `muse_companion.py results` opens it signed in.
- `MUSE_RESULTS_PUBLIC=true` in `.env` lets the results page be read online without the token.
- **The token is in `.env` too.** `MUSE_TOKEN` there is the online server's password, for Muse, the hooks and the web app's sign-in. A deploy writes the token it uses into `.env`, and you can choose your own (16 characters or more).
- **Keys in `.env`.** `DEEPGRAM_API_KEY` and `ANTHROPIC_API_KEY` go in a `.env` file in the repository's folder (copy `.env.example`); it is in `.gitignore`. Keys saved in `~/.config/muse` by earlier versions still work.
- A redeploy now turns Muse away from the old version for five minutes, since building the image with the worker's packages takes longer.

## 0.7.0 (2026-11-11)

- **Muse speaks first.** When a timer is up it says so ("Time's up! Tea."), waves and shows the label; when its battery falls to 20 % and to 10 % it asks to be plugged in (not at night); and the first time you pick it up in a morning it says good morning in its own words. Muse wakes, comes close and speaks, and holding BOOT answers it. `"speak_first": false` in `voice.json` turns it off.
- The server starts these through Deepgram's `InjectAgentMessage` and `InjectUserMessage`; Muse plays them like any reply, so the board needs no extra memory. Muse tells the server when it is picked up or woken (`awake`).
- `muse_status` also reports Muse's free memory.

## 0.6.0 (2026-11-11)

- **The AI has Muse's body and screen as tools.** `muse_do` performs moves in order (wave, giggle, cheer, dance, spin, shake_head, tilt_head, bounce, sway, smile, surprised, come_closer, step_back, sleep), `muse_show` puts up a sign, `muse_set` changes brightness and volume, alongside `claude_status`. "Dance for me", "come closer" or "good night" just work. Replaces the single `muse_action` tool.
- **The AI can sense things.** `muse_status` reads Muse's own sensors (battery and charging from the AXP2101, WiFi, position, room noise, brightness, volume), which Muse reports to the server; `time_now` gives the time and date; `timer` sets timers that end with a chirp, a wave and a sign; `remember` and `forget` keep notes about you between conversations.
- A thought cloud hangs over Muse's head while it thinks, in a conversation and while Claude Code is thinking.
- A dance: three bars of music with sways and a flourish each.

- **The server can run online**, so Muse works anywhere there is WiFi with no computer on. Online mode (`MUSE_TOKEN` set) requires a token from Muse and from the hooks and turns the local beacon off. `deploy_modal.py` deploys it to Modal; a `Dockerfile` covers other hosts.
- Muse looks for a local server first and otherwise connects to the online one over TLS, checking its certificate. `muse_companion.py pair` tells Muse the address and token once; Muse keeps them in NVS.
- `install_hooks.py --online`: the hooks run `muse_hook.py`, which keeps only what Muse shows (no prompts, file contents or commands) and sends it in the background.
- `test_voice.py --online` and `muse_companion.py demo --online` test the deployed server.
- Deploying again hands Muse over: the old version lets Muse go (`/retire`) so the new one can start.
- `blender/export_clips.py` also runs on a .blend that still has the model's older object names, and takes the standing pose from the rig wherever its rest pose is. The export from `muse.blend` is unchanged.

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
