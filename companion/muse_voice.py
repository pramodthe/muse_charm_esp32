# SPDX-License-Identifier: MIT
"""Voice chat for Muse, relayed through this computer.

Muse streams its microphone here while you hold the screen. This module passes
the audio to the Deepgram Voice Agent API (speech to text, the reply from an
LLM, text to speech) and streams the spoken reply back to Muse. The Deepgram
key stays on this computer.

    Muse  -> {"type":"talk_start"}, microphone audio (binary), {"type":"talk_end"}
             {"type":"talk_stop"}                      stop the reply that is playing
    Muse <-  {"type":"voice","state":"thinking|speaking|done|error"}
             {"type":"voice","action":"wave|giggle|cheer|dizzy"}
             reply audio (binary, 16 kHz 16-bit mono)

Key:      ~/.config/muse/deepgram_key, or the DEEPGRAM_API_KEY environment variable
Options:  ~/.config/muse/voice.json, for example
          {"voice": "aura-2-luna-en", "think_provider": "open_ai", "think_model": "gpt-4o-mini",
           "prompt": "You are Muse, ..."}
"""

import asyncio
import json
import os
import time

import muse_ws as ws

DEEPGRAM_URL = os.environ.get("MUSE_DEEPGRAM_URL", "wss://agent.deepgram.com/v1/agent/converse")
CONFIG_DIR = os.path.expanduser("~/.config/muse")
KEY_FILE = os.path.join(CONFIG_DIR, "deepgram_key")
OPTIONS_FILE = os.path.join(CONFIG_DIR, "voice.json")

SAMPLE_RATE = 16000
BYTES_PER_S = SAMPLE_RATE * 2
TAIL_SILENCE_S = 1.2     # after you let go, so Deepgram notices the end of the turn
KEEPALIVE_S = 7          # Deepgram wants a message at least every 8 s
IDLE_CLOSE_S = 120       # close the Deepgram connection after this much quiet
LEAD_S = 2.5             # how far ahead of playback reply audio is sent to Muse
CHUNK = 1600             # 50 ms of reply audio per message to Muse
HISTORY = 8              # recent messages carried over when reconnecting

DEFAULTS = {
    "voice": "aura-2-thalia-en",
    "listen_model": "nova-3",
    "think_provider": "anthropic",
    "think_model": "claude-haiku-4-5",
    "prompt": (
        "You are Muse, a tiny cheerful companion who lives on a little screen next to the user's "
        "computer. You are warm, playful and curious, and you talk like a kind friend. Everything you "
        "say is spoken aloud, so keep each reply to one or two short sentences and never use lists, "
        "markdown, emojis or stage directions. When it fits, call muse_action to wave, giggle, cheer "
        "or act dizzy. Claude Code, an AI coding assistant, often works on the same computer; if the "
        "user asks what Claude is doing or how their work is going, call claude_status and answer in "
        "a sentence."
    ),
}

ACTIONS = ["wave", "giggle", "cheer", "dizzy"]


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


def key_status():
    """(key, None) when a usable key is set up, else (None, what is wrong)."""
    key = os.environ.get("DEEPGRAM_API_KEY", "").strip()
    if not key:
        try:
            with open(KEY_FILE) as f:
                key = f.read().strip()
        except OSError:
            return None, "no Deepgram key: put it in " + KEY_FILE
    if not key:
        return None, KEY_FILE + " is empty"
    if any(c.isspace() for c in key) or len(key) < 20:
        return None, KEY_FILE + " does not look like a Deepgram key (it should hold just the key)"
    return key, None


def read_key():
    return key_status()[0]


def options():
    opts = dict(DEFAULTS)
    if os.path.exists(OPTIONS_FILE):
        try:
            with open(OPTIONS_FILE) as f:
                opts.update({k: v for k, v in json.load(f).items() if k in DEFAULTS and v})
        except (OSError, ValueError) as e:
            log("ignoring %s: %s" % (OPTIONS_FILE, e))
    return opts


def settings_message(opts, history):
    agent = {
        "listen": {"provider": {"type": "deepgram", "model": opts["listen_model"]}},
        "think": {
            "provider": {"type": opts["think_provider"], "model": opts["think_model"]},
            "prompt": opts["prompt"],
            "functions": [
                {
                    "name": "muse_action",
                    "description": "Make Muse do a short animation on its screen while it talks: wave for "
                                   "hello or goodbye, giggle at something funny, cheer at good news, dizzy "
                                   "when confused.",
                    "parameters": {
                        "type": "object",
                        "properties": {"action": {"type": "string", "enum": ACTIONS}},
                        "required": ["action"],
                    },
                },
                {
                    "name": "claude_status",
                    "description": "What Claude Code is doing on the user's computer right now: for each "
                                   "session, the project and whether Claude is thinking, running a tool, "
                                   "waiting for the user's permission, or idle.",
                    "parameters": {"type": "object", "properties": {}},
                },
            ],
        },
        "speak": {"provider": {"type": "deepgram", "model": opts["voice"]}},
    }
    if history:
        agent["context"] = {"messages": [{"type": "History", "role": r, "content": c} for r, c in history]}
    return {
        "type": "Settings",
        "audio": {
            "input": {"encoding": "linear16", "sample_rate": SAMPLE_RATE},
            "output": {"encoding": "linear16", "sample_rate": SAMPLE_RATE, "container": "none"},
        },
        "agent": agent,
    }


class Voice:
    """The voice conversation of one connected Muse."""

    def __init__(self, link, claude_status):
        self.link = link                    # the Muse connection (muse_companion.DeviceLink)
        self.claude_status = claude_status  # function returning a short JSON summary
        self.dg_reader = self.dg_writer = None
        self.connecting = None
        self.ready = False
        self.pending = []                   # microphone audio from before Deepgram was ready
        self.talking = False
        self.discard = False                # drop reply audio until the next reply starts
        self.history = []
        self.last_sent = self.last_activity = time.monotonic()
        self.tasks = []

    # ------------------------------------------------------------ from Muse
    def talk_start(self):
        self.talking = True
        self.discard = True
        self.pending = []
        self.last_activity = time.monotonic()
        self.link.flush_voice()
        key, problem = key_status()
        if not key:
            log("voice:", problem)
            self.link.send_json({"type": "voice", "state": "error", "error": problem[:120]})
            return
        if not self.dg_writer and not self.connecting:
            self.connecting = asyncio.ensure_future(self._connect())

    def mic(self, data):
        if not self.talking:
            return
        if self.ready:
            self._send(ws.BINARY, data)
        elif len(self.pending) < 400:      # about 16 s
            self.pending.append(data)

    def talk_end(self):
        if not self.talking:
            return
        self.talking = False
        self.last_activity = time.monotonic()
        self.tasks = [t for t in self.tasks if not t.done()]
        self.tasks.append(asyncio.ensure_future(self._tail()))

    def talk_stop(self):
        self.discard = True
        self.link.flush_voice()

    def close(self):
        for t in self.tasks + [self.connecting]:
            if t and not t.done():
                t.cancel()
        self.tasks, self.connecting = [], None
        if self.dg_writer:
            try:
                self.dg_writer.write(ws.frame(ws.CLOSE, b"\x03\xe8", masked=True))
                self.dg_writer.close()
            except Exception:
                pass
        self.dg_reader = self.dg_writer = None
        self.ready = False

    # ------------------------------------------------------------ Deepgram
    def _send(self, op, payload):
        if self.dg_writer:
            self.dg_writer.write(ws.frame(op, payload, masked=True))
            self.last_sent = time.monotonic()

    def _send_json(self, obj):
        self._send(ws.TEXT, json.dumps(obj).encode())

    def _fail(self, why):
        log("voice:", why)
        self.link.send_json({"type": "voice", "state": "error", "error": why[:120]})
        self.close()

    async def _connect(self):
        opts = options()
        try:
            self.dg_reader, self.dg_writer = await ws.connect(DEEPGRAM_URL, {"Authorization": "Token " + read_key()})
        except Exception as e:
            self.connecting = None
            self._fail("could not connect to Deepgram: %s" % (e or type(e).__name__))
            return
        log("voice: connected to Deepgram (%s, %s/%s)" % (opts["voice"], opts["think_provider"], opts["think_model"]))
        self._send_json(settings_message(opts, self.history))
        self.tasks.append(asyncio.ensure_future(self._read_loop()))
        self.tasks.append(asyncio.ensure_future(self._housekeeping()))
        self.connecting = None
        try:
            for _ in range(100):
                if self.ready or not self.dg_writer:
                    return
                await asyncio.sleep(0.1)
            self._fail("Deepgram did not answer in time")
        except asyncio.CancelledError:
            pass

    async def _tail(self):
        """A little silence after the turn, sent in real time."""
        silence = bytes(BYTES_PER_S // 25)
        for _ in range(100):               # wait up to 10 s for the connection
            if self.ready or not (self.dg_writer or self.connecting):
                break
            await asyncio.sleep(0.1)
        for _ in range(int(TAIL_SILENCE_S * 25)):
            if self.talking or not self.ready:
                return
            self._send(ws.BINARY, silence)
            await asyncio.sleep(0.04)

    async def _housekeeping(self):
        while self.dg_writer:
            await asyncio.sleep(1)
            now = time.monotonic()
            if not self.talking and now - self.last_activity > IDLE_CLOSE_S:
                log("voice: closing the quiet Deepgram connection")
                self.close()
                return
            if self.ready and now - self.last_sent > KEEPALIVE_S:
                self._send_json({"type": "KeepAlive"})

    async def _read_loop(self):
        reader, writer = self.dg_reader, self.dg_writer
        try:
            while True:
                op, data = await ws.read_message(reader, writer, masked_replies=True)
                if op == ws.CLOSE:
                    code = int.from_bytes(data[:2], "big") if len(data) >= 2 else 0
                    reason = data[2:].decode("utf-8", "replace")
                    if code not in (0, 1000) or self.talking:
                        log("voice: Deepgram closed the connection (%d %s)" % (code, reason))
                    break
                if op == ws.BINARY:
                    self._reply_audio(data)
                elif op == ws.TEXT:
                    self._event(json.loads(data))
        except asyncio.CancelledError:
            return
        except (asyncio.IncompleteReadError, ConnectionError, OSError, ValueError) as e:
            log("voice: connection to Deepgram lost (%s)" % (e or type(e).__name__))
        if self.dg_writer is writer:
            self.dg_reader = self.dg_writer = None
            self.ready = False

    def _reply_audio(self, data):
        if self.discard or self.talking:
            return
        self.last_activity = time.monotonic()
        for i in range(0, len(data), CHUNK):
            self.link.queue_voice("audio", data[i:i + CHUNK])

    def _event(self, msg):
        t = msg.get("type")
        if t == "SettingsApplied":
            self.ready = True
            for chunk in self.pending:
                self._send(ws.BINARY, chunk)
            self.pending = []
        elif t == "UserStartedSpeaking":
            self.discard = True
            self.link.flush_voice()
        elif t == "ConversationText":
            role, content = msg.get("role"), msg.get("content") or ""
            log("voice: %s: %s" % ("you" if role == "user" else "muse", content))
            self.history = (self.history + [(role, content[:400])])[-HISTORY:]
            self.last_activity = time.monotonic()
        elif t == "AgentThinking":
            self.link.send_json({"type": "voice", "state": "thinking"})
        elif t == "AgentStartedSpeaking":
            self.discard = False
            self.link.queue_voice("start", None)
        elif t == "AgentAudioDone":
            self.link.queue_voice("json", {"type": "voice", "state": "done"})
        elif t == "FunctionCallRequest":
            for f in msg.get("functions") or []:
                if f.get("client_side") is False:
                    continue
                self._function(f)
        elif t == "Error":
            self._fail("Deepgram error: %s" % (msg.get("description") or msg.get("code") or msg))
        elif t == "Warning":
            log("voice: Deepgram warning: %s" % (msg.get("description") or msg))

    def _function(self, f):
        name = f.get("name")
        try:
            args = json.loads(f.get("arguments") or "{}")
        except ValueError:
            args = {}
        if name == "muse_action" and args.get("action") in ACTIONS:
            log("voice: action", args["action"])
            self.link.queue_voice("json", {"type": "voice", "action": args["action"]})
            content = json.dumps({"ok": True})
        elif name == "claude_status":
            content = self.claude_status()
            log("voice: claude_status ->", content)
        else:
            content = json.dumps({"error": "unknown function"})
        self._send_json({"type": "FunctionCallResponse", "id": f.get("id"), "name": name, "content": content})
