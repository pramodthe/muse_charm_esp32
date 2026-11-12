#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test voice chat without Muse: pretend to be Muse, say a sentence to the
running companion server and save the spoken reply.

    python3 test_voice.py "Hello Muse, what is Claude doing?"
    python3 test_voice.py --online "Hello Muse!"     through the online server

On macOS the sentence is spoken with the built-in `say` voice; elsewhere pass
a 16 kHz 16-bit mono WAV file with --wav. The reply is saved to reply.wav.
"""

import argparse
import asyncio
import json
import os
import subprocess
import sys
import tempfile
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import muse_online    # noqa: E402
import muse_ws as ws  # noqa: E402

RATE = 16000
CHUNK = 1280  # 40 ms


def speech(text):
    path = os.path.join(tempfile.mkdtemp(), "speech.wav")
    subprocess.run(["say", "-o", path, "--data-format=LEI16@%d" % RATE, text], check=True)
    return path


def pcm_from_wav(path):
    with wave.open(path) as w:
        if w.getframerate() != RATE or w.getsampwidth() != 2 or w.getnchannels() != 1:
            sys.exit("%s must be 16 kHz, 16-bit, mono" % path)
        return w.readframes(w.getnframes())


async def run(pcm, url, token, out):
    reader, writer = await ws.connect(url, {"Authorization": "Bearer " + token} if token else None)
    start = time.monotonic()
    reply = bytearray()
    done = asyncio.Event()
    lookup = {"open": False}   # a lookup is running: the reply after "let me check" is still to come

    def stamp():
        return "%5.2f s" % (time.monotonic() - start)

    async def receive():
        while True:
            op, data = await ws.read_message(reader, writer, masked_replies=True)
            if op == ws.BINARY:
                if not reply:
                    print(stamp(), "first reply audio")
                reply.extend(data)
            elif op == ws.TEXT:
                msg = json.loads(data)
                if msg.get("type") in ("voice", "voice_ready"):
                    print(stamp(), msg)
                state = msg.get("state") if msg.get("type") == "voice" else None
                if state == "looking":
                    lookup["open"] = True
                elif state == "looked":
                    lookup["open"] = False
                elif state == "error" or (state == "done" and not lookup["open"]):
                    done.set()
            elif op == ws.CLOSE:
                done.set()
                return

    task = asyncio.ensure_future(receive())
    writer.write(ws.frame(ws.TEXT, b'{"type":"hello","name":"test_voice"}', masked=True))
    await asyncio.sleep(0.5)
    print(stamp(), "talking for %.1f s" % (len(pcm) / RATE / 2))
    writer.write(ws.frame(ws.TEXT, b'{"type":"talk_start"}', masked=True))
    for i in range(0, len(pcm), CHUNK):
        writer.write(ws.frame(ws.BINARY, pcm[i:i + CHUNK], masked=True))
        await asyncio.sleep(CHUNK / RATE / 2)
    writer.write(ws.frame(ws.TEXT, b'{"type":"talk_end"}', masked=True))
    print(stamp(), "let go")
    try:
        await asyncio.wait_for(done.wait(), 45)
    except asyncio.TimeoutError:
        print(stamp(), "no reply within 45 s")
    task.cancel()
    writer.close()
    if reply:
        with wave.open(out, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(bytes(reply))
        print("reply: %.1f s of audio saved to %s" % (len(reply) / RATE / 2, out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("text", nargs="?", default="Hello Muse! What is Claude doing right now?")
    ap.add_argument("--wav", help="16 kHz 16-bit mono WAV to send instead of speaking the text")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--online", action="store_true", help="use the online server in ~/.config/muse/server.json")
    ap.add_argument("--out", default="reply.wav")
    args = ap.parse_args()
    pcm = pcm_from_wav(args.wav or speech(args.text))
    url, token = "ws://127.0.0.1:%d/device" % args.port, ""
    if args.online:
        saved = muse_online.load()
        if not saved.get("url"):
            sys.exit("no online server saved in %s" % muse_online.SERVER_FILE)
        url, token = muse_online.device_url(saved["url"]), saved.get("token", "")
    asyncio.run(run(pcm, url, token, args.out))


if __name__ == "__main__":
    main()
