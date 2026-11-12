# Working on Muse

Muse is a desk companion: firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.8 (`firmware/`), a
Python companion server (`companion/`, standard library only) that runs on this computer or
online, and a web app served by that server. `README.md` explains what it does and
`companion/README.md` how the server works; read the section you are about to change.

## Where things are

- `firmware/main/`: one C file per job. `behavior.c` is what Muse does, `agent.c` the voice
  conversation, `companion.c` the link to the server, `update.c` firmware updates over WiFi.
- `companion/muse_companion.py`: the server (devices, hooks, HTTP). Around it: `muse_voice.py`
  (Deepgram Voice Agent and the AI's tools), `muse_chat.py` (typed chat and the shared
  conversation), `muse_world.py` (time, timers, reminders, notes), `muse_jobs.py` and
  `muse_worker.py` (background research), `muse_results.py` (the web app, one HTML string),
  `muse_firmware.py` (the image kept for updates).
- `assets/` and `blender/`: the character's frames and model. They are made by hand and exported
  with `blender/export_clips.py`; do not regenerate or edit them as part of other work.

## Commands

```bash
python3 companion/muse_companion.py                 # the server on this computer
python3 companion/deploy_modal.py --code-only       # redeploy the online server's code
idf.py -C firmware build                            # needs ESP-IDF v5.5 (source export.sh first)
python3 companion/muse_companion.py update          # send the build to Muse over WiFi
idf.py -C firmware -p /dev/cu.usbmodem1101 flash    # over the cable (the port name can change)
```

`python3 companion/deploy_modal.py` without `--code-only` also sends the keys in `.env` to the
host; that one is for the owner to run.

## Rules of the house

- Keys, the token and the online server's address live in `.env` and `~/.config/muse/`. Never
  put them in the repository, in commit messages or in logs, and do not print them.
- The server stays on the standard library. The research worker's packages (`deepagents`,
  `langchain-anthropic`) are the one exception and are only imported in `muse_worker.py`.
- A new tool for the AI goes in `TOOLS` and `run_tool()` in `muse_voice.py`; voice and typed chat
  share them. Say what it does in both prompts (`muse_voice.py`, `muse_chat.py`).
- Anything Muse says without being asked goes through `Voice.announce()`, which falls back to a
  sign when it cannot speak.
- The board has about 50 KB of internal memory free. Put buffers in PSRAM
  (`MALLOC_CAP_SPIRAM`), and check "internal RAM free" in the start-up log after a change.
- What the web app may do without the token is only reading results, and only when
  `MUSE_RESULTS_PUBLIC` is set. Everything else, and anything to do with firmware, needs it.
- Plain words in the README and in comments; update `README.md`, `companion/README.md` and
  `CHANGELOG.md` with the change, in the same commit.

## Checking a change

There is no test suite. Check server changes by running the server with a throwaway
`MUSE_DATA_DIR` and talking to it (`companion/test_voice.py`, or a small script that connects to
`/device` as Muse would). `MUSE_CHAT_FAKE=1` and `MUSE_WORKER_FAKE=1` stand in for the models.
Check firmware on the device and read its serial log.
