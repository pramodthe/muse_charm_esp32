# Companion server

A small Python program that runs on your computer, next to Muse. It does two jobs:

- **Voice chat.** While you hold Muse's BOOT button, Muse streams its microphone here. The server
  passes the audio to the [Deepgram Voice Agent API](https://developers.deepgram.com/docs/voice-agent)
  (speech to text, a reply from an LLM, text to speech) and streams the spoken reply back, with
  subtitles and gestures. The Deepgram key stays on your computer.
- **Claude Code companion.** Claude Code's [hooks](https://code.claude.com/docs/en/hooks) report
  each event here, and Muse shows what Claude is doing.

It uses the Python standard library only (Python 3.8 or later); there is nothing to install.
The one exception is the optional background worker, which needs `deepagents` and
`langchain-anthropic` (see [Background jobs](#background-jobs)).

## How it works

```
Claude Code ──hooks (curl)──> muse_companion.py <──websocket──> Muse
             127.0.0.1:8765     on your computer    over WiFi
                                      │
                                muse_voice.py <──wss──> Deepgram Voice Agent API
```

| File | |
| --- | --- |
| `muse_companion.py` | The server: Claude Code hooks on `127.0.0.1:8765/hook`, Muse's websocket on `:8765/device`, the UDP beacon on port 47800, a status page on `:8765/` |
| `muse_voice.py` | Voice chat: relays audio between Muse and Deepgram, raises the voice's pitch, sends subtitles and gestures |
| `muse_world.py` | The AI's time, timers, reminders, memory and its view of Muse's sensors |
| `muse_jobs.py` | Background jobs: starts `muse_worker.py`, keeps the results, tells Muse when one is done |
| `muse_worker.py` | One job, in its own process: a Deep Agent that searches the web and writes up what it finds |
| `muse_results.py` | The web app at `/results`: the chat, ideas, memory, Muse's status |
| `muse_chat.py` | The conversation shared by voice and web app, and the answers to typed messages (Anthropic API) |
| `muse_firmware.py` | The firmware image the online server keeps for Muse (updates over WiFi) |
| `muse_lookup.py` | Quick answers from the web for the `look_up` tool (Anthropic API with web search) |
| `muse_ws.py` | A small websocket implementation (server and client) |
| `muse_env.py` | Reads your keys from the repository's `.env` file |
| `install_hooks.py` | Adds or removes the Claude Code hooks |
| `test_voice.py` | Plays Muse's part from your computer, to test voice chat without Muse |
| `deploy_modal.py`, `modal_app.py` | Put the server online with Modal |
| `Dockerfile` | The server for any other container host |
| `muse_hook.py` | The Claude Code hook for an online server: keeps only what Muse shows |
| `muse_online.py` | Where the online server is (`~/.config/muse/server.json`) |

- The server announces itself on the local network with a UDP beacon, so Muse finds it without
  any setup and connects back over one websocket.
- Each Claude Code hook sends its event with `curl`, waits at most one second and always
  succeeds, so Claude Code behaves the same whether the server is running or not. Hook events are
  accepted from this computer only.
- The server turns the events into a mood and a short caption for Muse. Prompts, file contents and
  commands stay on your computer: Muse only receives the mood, a short description such as
  "Editing face.c" or the one-line description Claude writes for a shell command, and the project
  folder name.
- Reply audio is sent a couple of seconds ahead of playback, and each sentence's subtitle just
  before its audio, so Muse can time the words.

## Set up

1. Put Muse on the same WiFi network as your computer (setup opens on Muse's screen).
2. Start the server and leave it running:

   ```sh
   /usr/bin/python3 companion/muse_companion.py     # macOS
   python3 companion/muse_companion.py              # Linux
   ```

   Muse giggles and shows "Linked to ..." when it finds the server. On macOS, Muse can only
   connect if the firewall lets Python accept incoming connections: Apple's own
   `/usr/bin/python3` is allowed by default, while a Homebrew or python.org Python needs approving
   in System Settings → Network → Firewall → Options, again after each Python upgrade.

3. For voice chat, put your [Deepgram](https://console.deepgram.com) key in the `.env` file in
   the repository's folder:

   ```sh
   cp .env.example .env && chmod 600 .env      # then paste the key after DEEPGRAM_API_KEY=
   ```

   Or set `DEEPGRAM_API_KEY` in the server's environment. The server notices the key within a
   few seconds; `curl http://127.0.0.1:8765/` then shows `"voice": "ready"`. `.env` is in
   `.gitignore`, and the key is never sent to Muse. `muse_env.py` reads the file; a variable set
   in the environment wins over it. New to Deepgram? See
   [Setting up Deepgram](../README.md#setting-up-deepgram).

4. For the Claude Code companion, add the hooks (your other settings and hooks are kept, and a
   backup is saved as `~/.claude/settings.json.muse-backup`):

   ```sh
   python3 companion/install_hooks.py            # --print shows them, --remove takes them out
   ```

   New Claude Code sessions report to Muse; restart sessions that are already open.

## Online

The server can run on the internet instead, so Muse works without your computer; see
[the README](../README.md#without-your-computer-the-server-online) for the steps. What differs in
online mode (the `MUSE_TOKEN` environment variable is set):

- Muse (`/device`) and the hooks (`/hook`) must send `Authorization: Bearer <token>`; without it
  the status page shows nothing but the server's name.
- No beacon. Muse learns the address and token from a local server run as
  `muse_companion.py pair`, and keeps them in its own storage.
- The Deepgram key comes from `DEEPGRAM_API_KEY`, the Anthropic key for background jobs from
  `ANTHROPIC_API_KEY`, and voice options from `MUSE_VOICE` (the same JSON as `voice.json`). `MUSE_TZ` sets the time zone and `MUSE_DATA_DIR` the folder for what
  Muse remembers; `modal_app.py` sets both (your computer's time zone, and a Modal volume).
- The hooks run `muse_hook.py`, which strips each event down to what Muse shows before anything
  leaves your computer, and sends it in the background.
- After changing the server's code, `python3 companion/deploy_modal.py --code-only` deploys it
  again. Modal keeps the old version running for as long as Muse stays connected to it, so the
  script first asks the old one (`POST /retire`) to let Muse go and turn it away for five minutes;
  Muse reconnects by itself and lands on the new version.

### Firmware updates

`python3 companion/muse_companion.py update [file]` sends a firmware build (by default
`firmware/build/muse_charm.bin`) to the online server and waits while Muse installs it;
`--no-wait` only sends it. On the server, in online mode and always with the token:

| | |
| --- | --- |
| `POST /firmware` | The image as the body. `muse_firmware.describe()` refuses anything that is not this project's intact ESP32-S3 app image. It is kept as `firmware.bin` in the data folder |
| `GET /firmware.bin` | The image, for Muse |
| `GET /firmware.json` | `{"server": {"id", "version", "size", "built"}, "muse": {"id", "version", "update": {"state", "percent"}}}` |

An image's `id` is the SHA-256 that ESP-IDF appends to it. Muse sends the id of the image it is
running in its `hello` (`"fw"`, with `"ver"`); if the server's is another one, the server sends
`{"type":"firmware","id":...,"size":...}`, on connecting and when a new image is uploaded. Muse
reports back with `{"type":"update","state":...}`: `downloading` (with `percent`), `restarting`,
`kept` (the new image linked, so it stays), `failed` or `refused` (this image was dropped after
a trial before). The firmware's side is `firmware/main/update.c`.

## Voice options

Create `~/.config/muse/voice.json` to change how Muse talks, for example
`{"voice": "aura-2-luna-en", "think_provider": "open_ai", "think_model": "gpt-4o-mini"}`:

| Key | Default | |
| --- | --- | --- |
| `voice` | `aura-2-thalia-en` | Any Deepgram Aura voice |
| `pitch` | `1.18` | How much higher Muse speaks (also a little quicker); `1` for the voice unchanged |
| `listen_model` | `nova-3` | Deepgram speech-to-text model |
| `think_provider` / `think_model` | `anthropic` / `claude-haiku-4-5` | Any Deepgram think provider and model |
| `prompt` | a short, playful Muse | Muse's personality |
| `timezone` | the server's own | An IANA name such as `Australia/Sydney`, for the time tool |
| `speak_first` | `true` | `false`: no spoken timers, reminders, battery warnings or good mornings (timers and reminders still chirp and show a sign) |
| `chat_model` | `claude-haiku-5-5` | The model that answers typed messages (`muse_chat.py`) |
| `lookup_model` | `claude-haiku-5-5` | The model for quick answers from the web (`muse_lookup.py`) |
| `lookup_searches` | `3` | Web searches one quick question may use |
| `place` | the city in the time zone's name | Where the user is, for the weather and local questions |

The AI's tools are declared in `TOOLS` and carried out by `run_tool()` in `muse_voice.py`, for
the voice and the typed chat alike (`run_tool_async()` for one that waits on the network):

- `muse_do` (moves such as wave, dance, come_closer or sleep, one or several in order),
  `muse_show` (a short sign on the screen) and `muse_set` (brightness and volume), which the
  firmware performs.
- `muse_status`: what Muse last reported about itself (battery, WiFi, position, room noise ...).
  Muse sends a `status` message at the start of each conversation and once a minute.
- `time_now`, `timer`, `reminder`, `remember` and `forget`, in `muse_world.py`. Notes are kept in
  `memory.json` in `~/.config/muse` (or `MUSE_DATA_DIR`) and added to the prompt when a
  conversation starts; reminders are kept in `reminders.json` there.
- `claude_status`: what your Claude Code sessions are doing.
- `look_up`: a quick answer from the web, in `muse_lookup.py`. One request to the Anthropic API
  with the basic web search tool (`web_search_20250305`, the fastest), low effort, and a prompt
  that asks for one or two spoken sentences in local units. It runs in a thread. In a spoken
  conversation the server sends Muse `{"type":"voice","state":"looking"}` and then `"looked"`,
  and the firmware keeps its thought cloud up between "let me check" and the answer (Deepgram
  sends both as one reply). The pages the answer cites are added to the conversation as
  `{"who": "muse", "lookup": ..., "sources": [...]}`, which the web app shows as links.
  `MUSE_LOOKUP_FAKE=1` answers without the API, for tests.
- `work_on` and `work_status`: background jobs, below.

## Background jobs

`work_on` calls `Jobs.start()` in `muse_jobs.py`. The job is written to
`jobs/<id>.json` in `~/.config/muse` (or `MUSE_DATA_DIR`) and `muse_worker.py` is started on it
as a separate process, so the server keeps answering Muse and stays free of the worker's
packages. The worker builds a [Deep Agent](https://github.com/langchain-ai/deepagents) with
Anthropic's web search tool and an in-memory `StateBackend` (no shell, no real files), and writes
`status`, `summary` and `report` back into the job file. When the process ends, `Hub.on_job_done()`
has Muse announce the summary (see [Speaking first](#speaking-first)).

| | |
| --- | --- |
| Key | `ANTHROPIC_API_KEY` in `.env`, or in the environment |
| Python for the worker | This one if it has `deepagents`; otherwise `uv run --with deepagents --with langchain-anthropic`; or the one named in `MUSE_WORKER_PYTHON` |
| Limits | 12 minutes a job, 2 at once, 8 web searches a job, the newest 30 kept |
| Model | `worker_model` in `voice.json` (default `anthropic:claude-sonnet-5-5`) |
| Testing | `MUSE_WORKER_FAKE=1` makes the worker return a canned result without a model |

The write-ups are read in the web app at `/results` on the server (`muse_companion.py results`
opens it), laid out as a chat. `GET /avatar` serves `companion/avatar.webp` (or `.gif`, `.png`,
`.jpg`), if you have put a picture there, as Muse's picture in it. The page itself holds no data. It fetches `/results.json` and `/results/<id>.json`
for the jobs when it is not signed in, and otherwise `/feed.json`: the conversation, the jobs
and Muse's state, timers and memory in one answer. It posts `{"text": ...}` to `/chat` (`{"remove": [t, ...]}` there takes entries out of the conversation) for a
typed message, `{"task": ...}` to `/ask` to start a job directly (which calls `Hub.start_job()`,
the same as the `work_on` tool) and `{"add": ...}` or `{"forget": ...}` to `/memory`, and `{"cancel": "<id>"}` to `/reminders`.

`muse_chat.Conversation` keeps what was said in `chat.json`, from both sides: the voice adds each
spoken turn, and `muse_chat.reply()` adds each typed one. `reply()` asks Claude with the same
tools as the voice (`run_tool()` in `muse_voice.py` carries them out for both). In a typed
conversation a move, a sign or a setting goes straight to every connected Muse as a `voice`
message, and Muse performs moves that arrive outside a spoken turn by themselves. All of these need the token online; on your own computer the
page is open, and only that computer may post. Posts must carry `Content-Type: application/json`
and an `X-Muse` header, which a form on another website cannot send. `MUSE_RESULTS_PUBLIC=true` in `.env` (then deploy again) drops that: anyone who has
the server's address can then read your requests and write-ups (the two `/results` addresses
only), so leave it off if they might be private. The command passes the token after the `#` of the address, which browsers do not
send to servers, and the page keeps it in the browser's storage.

## Speaking first

`Voice.announce()` makes Muse say something nobody asked for. The server opens the Deepgram
connection if it has to, and sends either `InjectAgentMessage` (a line spoken word for word) or
`InjectUserMessage` (a note the AI answers in its own words). The audio goes to Muse like any
reply, after `{"type":"voice","state":"speaking"}`, with a sign and moves alongside. A connection
opened only for this is closed again afterwards, because Deepgram drops one that never gets
microphone audio.

| Trigger | From | Says |
| --- | --- | --- |
| A timer is up | `Hub.alert()` | `timer_line()`: "Time's up! Tea." |
| A reminder is due (`Reminders.run()` looks every 10 s) | `Hub.remind()` | `reminder_line()`: "Hey, reminder time! Call mum." With no Muse connected it is kept in `state.json` and said by `Hub.catch_up()` 8 s after one connects |
| A background job ends | `Hub.on_job_done()` | "I'm done!" and the job's summary, or that it could not finish |
| `status` reports a battery at or under 20 %, then 10 %, not charging | `Hub.on_status()` | `battery_line()`; not between 10 pm and 8 am |
| Muse sends `{"type":"awake"}` (picked up, woken by touch, switched on) | `Hub.on_awake()` | The AI's own good morning, once a day between 5 and 11 am (`state.json` remembers the day). `briefing_note()` adds today's reminders, missed ones and the research finished since the last briefing to the note it is handed |

An announcement waits for a pause if a conversation is under way (a greeting is dropped instead).
If Deepgram cannot be reached or sends no audio, it is tried again (three times in all), and the
log says how much was said ("said 2.1 s to Muse"). If Muse still cannot speak, it gets
`{"type":"alert","text":"..."}`: chirps, a wave and the sign.

## Claude Code events

| Claude Code | Muse |
|---|---|
| A session starts | Waves, shows the project name |
| Thinking | A thought cloud overhead, "Thinking..." |
| Running a tool | Reads the caption: "Editing face.c", "Run the tests" |
| Waiting for your permission | Waves for attention (up to three times): "OK to flash the firmware?" |
| A tool call fails | A quiet, puzzled look |
| Finished its reply | Cheers: "Done!" |
| The turn failed (API error) | Shakes its head |
| Compacting its memory | Squeezes: "Tidying up memory" |
| The session ends | Waves goodbye |

With several sessions open, Muse shows the one that most needs you.

## Try it

```sh
python3 companion/muse_companion.py demo                         # a made-up Claude Code session
python3 companion/test_voice.py "Hello Muse, tell me a joke."    # voice chat without Muse
curl http://127.0.0.1:8765/                                      # what the server sees
```

`test_voice.py` speaks the sentence with macOS `say` (or sends `--wav file.wav`, 16 kHz 16-bit
mono) and saves Muse's spoken reply to `reply.wav`.

## Troubleshooting

- **Muse finds the server but never links** (the serial log shows "Error read response for
  Upgrade header"): the computer's firewall is blocking Python; see step 2.
- **Muse never finds the server.** Some networks (guest WiFi, many offices) block broadcasts or
  traffic between devices. Set the server address by hand in `idf.py menuconfig` → Muse charm →
  Claude Code companion, for example `ws://192.168.1.20:8765/device`.
- **Muse shakes its head when you hold BOOT:** the server's output says why, for example a
  missing or unusable key, or an error from Deepgram.
- **The server logs nothing when Claude works.** Check that `~/.claude/settings.json` has the
  hooks (`install_hooks.py --print` shows what they look like) and that the session was started
  after installing them.
- **Muse stays on "Thinking..."** after you interrupt Claude: interrupted turns send no "done"
  event, so the mood clears by itself after five minutes, or at your next prompt.

The server talks to Muse over plain websocket on your local network, without pairing. Anyone on the
same network could send Muse captions or reach the voice relay; use it on networks you trust.
