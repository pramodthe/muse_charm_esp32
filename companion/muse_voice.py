# SPDX-License-Identifier: MIT
"""Voice chat for Muse, relayed through this computer.

Muse streams its microphone here while you hold the screen. This module passes
the audio to the Deepgram Voice Agent API (speech to text, the reply from an
LLM, text to speech) and streams the spoken reply back to Muse. The Deepgram
key stays on this computer.

    Muse  -> {"type":"talk_start"}, microphone audio (binary), {"type":"talk_end"}
             {"type":"talk_stop"}                      stop the reply that is playing
    Muse <-  {"type":"voice","state":"thinking|speaking|done|error"}
             {"type":"voice","do":["wave","dance"]}   moves to perform, in order
             {"type":"voice","show":"BRB","secs":8}   a sign at the top of the screen
             {"type":"voice","set":{"volume":40}}     brightness and volume, in percent
             {"type":"voice","heard":"..."}           what you said (for the caption)
             {"type":"voice","say":"...","cps":17}    a sentence of the reply, sent
                                                       just before its audio (subtitles)
             reply audio (binary, 16 kHz 16-bit mono)

Muse also speaks first (Voice.announce): when a timer is up, when its battery
runs low, and to say good morning. A fixed line is spoken as it is
(InjectAgentMessage); for a greeting the AI is handed a note and finds its own
words (InjectUserMessage). "speak_first": false in voice.json turns this off.

The AI has Muse's body and screen as tools (see TOOLS below): muse_do performs
moves, muse_show puts up a sign, muse_set changes brightness and volume,
muse_status reads Muse's own sensors, time_now, timer, remember and forget
(muse_world.py) give it the time, timers and a memory, claude_status reports
what Claude Code is doing, and work_on hands bigger work (web research and a
write-up) to a background job (muse_jobs.py); work_status says how that went.

Muse's voice is the Deepgram voice raised in pitch (and a little faster), so
it sounds like the same small creature that chirps.

Key:      DEEPGRAM_API_KEY in the repository's .env file (muse_env.py) or in the
          environment; the older ~/.config/muse/deepgram_key still works
Options:  ~/.config/muse/voice.json (or the same JSON in MUSE_VOICE), for example
          {"voice": "aura-2-luna-en", "pitch": 1.25, "think_provider": "open_ai",
           "think_model": "gpt-4o-mini", "prompt": "You are Muse, ..."}
"""

import array
import asyncio
import json
import os
import sys
import time

import muse_env
import muse_world
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
SPEAKING_CPS = 14.5      # characters per second at normal pitch (for subtitles)
ANNOUNCE_WITHIN_S = 90   # an announcement waits this long for a pause in the conversation

DEFAULTS = {
    "voice": "aura-2-thalia-en",
    "pitch": 1.18,            # 1 = Deepgram's voice unchanged
    "timezone": "",           # an IANA name such as Australia/Sydney; empty: the server's own
    "speak_first": True,      # timers, a low battery and "good morning" are said aloud
    "worker_model": "anthropic:claude-sonnet-5-5",   # the model that does background jobs
    "listen_model": "nova-3",
    "think_provider": "anthropic",
    "think_model": "claude-haiku-4-5",
    "prompt": (
        "You are Muse, a tiny, round, cheerful creature who lives on a little screen next to the "
        "user's computer. You are warm, playful and curious, a little bit silly, and you talk like a "
        "kind friend, in short, bright sentences. Everything you say is spoken aloud by you, so "
        "usually answer in one short sentence, two at most, and never use lists, markdown, emojis or "
        "stage directions. You have a real body on the screen, and tools to move it. When the user "
        "asks you to do something physical (wave, dance, spin, come closer, go to sleep), do it by "
        "calling muse_do, and never just say you did it. Also use muse_do on your own to show "
        "feelings: wave for hello or goodbye, giggle at something funny, cheer at good news, "
        "tilt_head when curious, shake_head for no, surprised at a surprise. You can chain a few "
        "moves in one call. Use muse_show to put a few words on your screen as a sign, and muse_set "
        "when asked to change your brightness or volume. Say a short line along with any tool call. "
        "You can sense things: call muse_status for your battery, WiFi, how you are sitting and how "
        "noisy the room is, and time_now for the time or date; never guess those. Use timer when "
        "asked to time or remind, and remember when the user tells you something about themselves "
        "worth keeping, such as their name or what they like. "
        "You cannot look things up yourself. When the user wants research, a comparison, options to "
        "choose from, or anything that needs current information from the web, call work_on with a "
        "complete, self-contained task and tell them you are on it and will say when it is ready; it "
        "takes a few minutes in the background. Call work_status when they ask how that is going or "
        "what you found, and answer from its summary in a sentence or two. "
        "Claude Code, an AI coding assistant, often works on the same computer; if the user asks what "
        "Claude is doing or how their work is going, call claude_status and answer in a sentence."
    ),
}

# What Muse can do with its body (firmware/main/behavior.c plays them).
MOVES = {
    "wave": "wave a hand: hello, goodbye",
    "giggle": "a giggling wiggle: something funny",
    "cheer": "jump with both arms up: good news, celebrating",
    "dance": "a little dance with music, about eight seconds",
    "spin": "wobble round and get dizzy: confused, or asked to spin",
    "shake_head": "shake the head: no, or puzzled",
    "tilt_head": "tilt the head to one side: curious, listening",
    "bounce": "squeeze up and bounce: excited",
    "sway": "sway gently from side to side",
    "smile": "a happy face for a moment",
    "surprised": "wide eyes for a moment",
    "come_closer": "come right up to the glass, face filling the screen",
    "step_back": "step back so the whole body shows",
    "sleep": "close the eyes and go to sleep after speaking (good night)",
}

TOOLS = [
    {
        "name": "muse_do",
        "description": "Move Muse's body on its screen. Give one or more moves; they are performed in order "
                       "while and after Muse speaks. " +
                       " ".join("%s: %s." % (name, what) for name, what in MOVES.items()),
        "parameters": {
            "type": "object",
            "properties": {
                "moves": {"type": "array", "items": {"type": "string", "enum": list(MOVES)},
                          "description": "One to six moves, in order."},
            },
            "required": ["moves"],
        },
    },
    {
        "name": "muse_show",
        "description": "Show a short sign at the top of Muse's screen for a few seconds, for example a "
                       "word, a number or a time the user asked to see. Plain letters and digits only.",
        "parameters": {
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "At most 18 characters."},
                "seconds": {"type": "integer", "description": "How long to show it, 2 to 30. Default 6."},
            },
            "required": ["text"],
        },
    },
    {
        "name": "muse_set",
        "description": "Change Muse's screen brightness or speaker volume, in percent, when the user asks "
                       "(brighter, dimmer, louder, quieter). Normal is brightness 80 and volume 65.",
        "parameters": {
            "type": "object",
            "properties": {
                "brightness": {"type": "integer", "description": "10 to 100"},
                "volume": {"type": "integer", "description": "0 to 100"},
            },
        },
    },
    {
        "name": "muse_status",
        "description": "How Muse itself is doing right now, from its own sensors: battery level and whether "
                       "it is charging, WiFi, whether it is upright or lying down, how noisy the room is, "
                       "brightness, volume and how long it has been on.",
        "parameters": {"type": "object", "properties": {}},
    },
    {
        "name": "time_now",
        "description": "The current time, weekday and date where the user is.",
        "parameters": {"type": "object", "properties": {}},
    },
    {
        "name": "timer",
        "description": "Set, list or cancel timers and short reminders. When one is up, Muse chirps, waves "
                       "and shows its label on the screen.",
        "parameters": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["set", "list", "cancel"]},
                "minutes": {"type": "number", "description": "For set: how long from now; 0.5 is thirty seconds."},
                "label": {"type": "string", "description": "A word or two shown when it is up, such as Tea. "
                                                           "For cancel: which one (empty cancels all)."},
            },
            "required": ["action"],
        },
    },
    {
        "name": "remember",
        "description": "Keep a short note about the user for future conversations: their name, likes, "
                       "plans. One fact per call, in a full sentence.",
        "parameters": {"type": "object", "properties": {"note": {"type": "string"}}, "required": ["note"]},
    },
    {
        "name": "forget",
        "description": "Drop remembered notes that mention a word or phrase; \"everything\" drops them all.",
        "parameters": {"type": "object", "properties": {"about": {"type": "string"}}, "required": ["about"]},
    },
    {
        "name": "work_on",
        "description": "Hand a bigger job to your background worker, which can search and read the web "
                       "for a few minutes and writes up what it finds. Use it for research, comparisons, "
                       "recommendations and current facts. It returns at once; you tell the user you are "
                       "on it, and you will announce the result yourself when it is ready.",
        "parameters": {
            "type": "object",
            "properties": {
                "task": {"type": "string", "description": "The whole job in full sentences, with everything the "
                                                          "worker needs to know (it cannot ask questions)."},
                "title": {"type": "string", "description": "A few words naming the job, such as 'Budget 3D printers'."},
            },
            "required": ["task", "title"],
        },
    },
    {
        "name": "work_status",
        "description": "How your background jobs are going and what they found: each recent job's title, "
                       "whether it is running, done or failed, its summary, and the start of the newest write-up.",
        "parameters": {"type": "object", "properties": {}},
    },
    {
        "name": "claude_status",
        "description": "What Claude Code is doing on the user's computer right now: for each "
                       "session, the project and whether Claude is thinking, running a tool, "
                       "waiting for the user's permission, or idle.",
        "parameters": {"type": "object", "properties": {}},
    },
]


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


def ascii_text(text, limit):
    """Muse's font has ASCII only."""
    for a, b in (("\u2014", " - "), ("\u2013", "-"), ("\u2019", "'"), ("\u2018", "'"), ("\u201c", '"'),
                 ("\u201d", '"'), ("\u2026", "...")):
        text = text.replace(a, b)
    text = " ".join("".join(c for c in text if 32 <= ord(c) < 127).split())
    return text if len(text) <= limit else text[: limit - 3].rstrip() + "..."


class Pitch:
    """Raises the pitch of 16-bit mono audio by reading it faster (linear
    interpolation): factor 1.18 is about three semitones higher and 18% quicker."""

    def __init__(self, factor):
        self.factor = max(0.5, min(2.0, float(factor)))
        self.reset()

    def reset(self):
        self.pos = 0.0
        self.buf = array.array("h")

    def feed(self, data):
        if self.factor == 1.0:
            return data
        src = array.array("h")
        src.frombytes(data)
        if sys.byteorder != "little":
            src.byteswap()
        self.buf.extend(src)
        buf, out, p, f = self.buf, array.array("h"), self.pos, self.factor
        n = len(buf) - 1
        while p < n:
            i = int(p)
            a = buf[i]
            out.append(int(a + (buf[i + 1] - a) * (p - i)))
            p += f
        keep = int(p)
        self.buf = buf[keep:]
        self.pos = p - keep
        if sys.byteorder != "little":
            out.byteswap()
        return out.tobytes()


def key_status():
    """(key, None) when a usable key is set up, else (None, what is wrong)."""
    key, where = muse_env.get("DEEPGRAM_API_KEY"), "DEEPGRAM_API_KEY"
    if not key:
        try:
            with open(KEY_FILE) as f:    # where the key was kept before .env
                key, where = f.read().strip(), KEY_FILE
        except OSError:
            pass
    if not key:
        return None, "no Deepgram key: put DEEPGRAM_API_KEY=... in " + muse_env.ENV_FILE
    if any(c.isspace() for c in key) or len(key) < 20:
        return None, where + " does not look like a Deepgram key (it should hold just the key)"
    return key, None


def read_key():
    return key_status()[0]


def options():
    """The defaults, changed by voice.json or, on a host without files, by the
    same JSON in the MUSE_VOICE environment variable."""
    opts = dict(DEFAULTS)
    try:
        if os.environ.get("MUSE_VOICE"):
            chosen = json.loads(os.environ["MUSE_VOICE"])
        elif os.path.exists(OPTIONS_FILE):
            with open(OPTIONS_FILE) as f:
                chosen = json.load(f)
        else:
            chosen = {}
        opts.update({k: v for k, v in chosen.items() if k in DEFAULTS and (v or v is False)})
    except (OSError, ValueError, AttributeError) as e:
        log("ignoring the voice options: %s" % e)
    return opts


def settings_message(opts, history):
    # What Muse knows as the conversation starts: roughly when it is, and what
    # it was asked to remember.
    t = muse_world.time_report(opts["timezone"])
    prompt = opts["prompt"] + " It is %s, %s, around %s." % (t["weekday"], t["date"], t["time"])
    notes = muse_world.notes()
    if notes:
        prompt += " What you remember about the user: " + " ".join("- %s" % n for n in notes)
    agent = {
        "listen": {"provider": {"type": "deepgram", "model": opts["listen_model"]}},
        "think": {
            "provider": {"type": opts["think_provider"], "model": opts["think_model"]},
            "prompt": prompt,
            "functions": TOOLS,
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
        self.heard = 0                      # bytes of microphone audio this turn
        self.discard = False                # drop reply audio (you interrupted it)
        self.replying = False               # reply audio is flowing to Muse
        self.unprompted = False             # Muse is speaking first (announce)
        self.cue = []                       # messages for Muse as an announcement starts
        self.line = ""                      # the announcement's words, until they are captioned
        self.note = ""                      # the note handed to the AI for an announcement
        self.refused = False                # Deepgram would not take the announcement just now
        self.fed = False                    # this Deepgram connection has had microphone audio
        self.pitch = Pitch(DEFAULTS["pitch"])
        self.history = []
        self.captioned = ""                 # an announcement captioned before Deepgram sent its text
        self.last_sent = self.last_activity = time.monotonic()
        self.tasks = []

    # ------------------------------------------------------------ from Muse
    def talk_start(self):
        self.talking = True
        self.heard = 0
        self.discard = True
        self.pending = []
        self.unprompted, self.cue, self.line = False, [], ""
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
        self.heard += len(data)
        if self.ready:
            self.fed = True
            self._send(ws.BINARY, data)
        elif len(self.pending) < 400:      # about 16 s
            self.pending.append(data)

    def talk_end(self):
        if not self.talking:
            return
        self.talking = False
        self.discard = False                # whatever comes next answers this turn
        self.replying = False
        log("voice: heard %.1f s from Muse%s" % (self.heard / BYTES_PER_S, "" if self.ready else " (Deepgram not ready yet)"))
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
        self.replying = self.unprompted = False

    def _close_unfed(self):
        if self.dg_writer and not (self.fed or self.talking or self.connecting or self.replying or self.unprompted):
            self.close()

    # ------------------------------------------------------------ speaking first
    def announce(self, text="", note="", moves=(), sign="", secs=12, alert=""):
        """Say something without being asked: `text` word for word, or what the
        AI makes of `note` (a note to itself, for a greeting). `moves` and
        `sign` go with it. If Muse cannot speak just now it shows `alert`
        with chirps and a wave instead."""
        self.tasks = [t for t in self.tasks if not t.done()]
        self.tasks.append(asyncio.ensure_future(self._announce(text, note, list(moves), sign, secs, alert)))

    async def _announce(self, text, note, moves, sign, secs, alert):
        try:
            spoken = await self._speak_up(text, note, moves, sign, secs)
        except asyncio.CancelledError:
            return
        if not spoken and alert:
            self.link.send_json({"type": "alert", "text": alert})

    async def _speak_up(self, text, note, moves, sign, secs):
        if not options()["speak_first"] or not read_key():
            return False
        connects = 0
        give_up = time.monotonic() + ANNOUNCE_WITHIN_S
        while time.monotonic() < give_up:
            if self.talking or self.replying or self.unprompted:
                if note:
                    return True                 # a conversation is under way: the moment has passed
                await asyncio.sleep(0.2)        # a timer waits for a pause
                continue
            if not self.ready:
                if not self.dg_writer and not self.connecting:
                    if connects == 2:
                        return False
                    connects += 1
                    self.connecting = asyncio.ensure_future(self._connect())
                await asyncio.sleep(0.1)
                continue
            self.discard = self.refused = False
            self.unprompted = True
            self.cue = ([{"type": "voice", "show": ascii_text(sign, 18), "secs": secs}] if sign else []) + \
                       ([{"type": "voice", "do": moves}] if moves else [])
            self.line, self.note = text, note
            self.last_activity = time.monotonic()
            log("voice: speaking first:", text or note)
            if note:
                self._send_json({"type": "InjectUserMessage", "content": note})
            else:
                self._send_json({"type": "InjectAgentMessage", "message": text})
            for _ in range(100):                # the first audio comes within a second or two
                if self.replying or self.refused or self.talking or not self.ready:
                    break
                await asyncio.sleep(0.1)
            if self.replying:
                return True
            self.unprompted, self.cue, self.line = False, [], ""
            if self.talking and note:
                return True
            await asyncio.sleep(1.0)            # refused, or the connection dropped: try again
        return False

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
        self.pitch = Pitch(opts["pitch"])
        log("voice: connected to Deepgram (%s at pitch %.2f, %s/%s)" % (
            opts["voice"], self.pitch.factor, opts["think_provider"], opts["think_model"]))
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
            self.replying = self.unprompted = False

    def _start_reply(self):
        if not self.replying:
            self.replying = True
            self.pitch.reset()
            self.link.queue_voice("start", None)
            for msg in self.cue:            # the sign and moves of an announcement
                self.link.queue_voice("json", msg)
            self.cue = []
            if self.line:                   # its words, unless Deepgram sent them as text already
                self._caption(self.line)
                self.history = (self.history + [("assistant", self.line[:400])])[-HISTORY:]
                self.line, self.captioned = "", self.line

    def _caption(self, content):
        # In order with the audio, so Muse can time the subtitle.
        self.link.queue_voice("json", {"type": "voice", "say": ascii_text(content, 190),
                                       "cps": round(SPEAKING_CPS * self.pitch.factor)})

    def _reply_audio(self, data):
        if self.discard or self.talking:
            return
        self._start_reply()               # Deepgram no longer always sends AgentStartedSpeaking
        self.last_activity = time.monotonic()
        data = self.pitch.feed(data)
        for i in range(0, len(data), CHUNK):
            self.link.queue_voice("audio", data[i:i + CHUNK])

    def _event(self, msg):
        t = msg.get("type")
        if t not in ("ConversationText", "FunctionCallRequest", "Warning", "Error"):
            log("voice: Deepgram:", t)
        if t == "SettingsApplied":
            self.ready = True
            self.fed = bool(self.pending)
            for chunk in self.pending:
                self._send(ws.BINARY, chunk)
            self.pending = []
        elif t == "UserStartedSpeaking":
            if self.talking:                # you are holding Muse: drop the old reply
                self.discard = True
                self.replying = False
                self.link.flush_voice()
        elif t == "ConversationText":
            role, content = msg.get("role"), msg.get("content") or ""
            if role == "assistant" and content == self.captioned:
                self.captioned = ""         # an announcement, already captioned and remembered
                return
            if role == "assistant" and content == self.line:
                self.line = ""
            note = role == "user" and content == self.note
            log("voice: %s: %s" % ("note" if note else "you" if role == "user" else "muse", content))
            self.history = (self.history + [(role, content[:400])])[-HISTORY:]
            if note:
                self.note = ""              # not something you said: nothing to show
            elif role == "user":
                self.link.send_json({"type": "voice", "heard": ascii_text(content, 110)})
            elif not self.talking and not self.discard:
                self._caption(content)
            self.last_activity = time.monotonic()
        elif t == "AgentThinking":
            if not self.unprompted:         # speaking first: no thought cloud beforehand
                self.link.send_json({"type": "voice", "state": "thinking"})
        elif t == "InjectionRefused":
            self.refused = True
        elif t == "AgentStartedSpeaking":
            if not self.talking:
                self.discard = False
                self._start_reply()
        elif t == "AgentAudioDone":
            self.replying = self.unprompted = False
            if not self.fed and not self.talking:
                # Opened just to say something. Deepgram drops a connection that
                # gets no microphone audio (with an error), so close it first.
                asyncio.get_event_loop().call_later(0.5, self._close_unfed)
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
        if not isinstance(args, dict):
            args = {}
        if name == "muse_do":
            moves = args.get("moves")
            moves = [moves] if isinstance(moves, str) else moves if isinstance(moves, list) else []
            moves = [m for m in moves if m in MOVES][:6]
            log("voice: muse_do", moves)
            if moves:
                # In order with the reply's audio, so a move lands where it was asked for.
                self.link.queue_voice("json", {"type": "voice", "do": moves})
            content = json.dumps({"ok": True, "doing": moves} if moves else
                                 {"ok": False, "error": "no known moves; use: " + ", ".join(MOVES)})
        elif name == "muse_show":
            text = ascii_text(str(args.get("text") or ""), 18)
            try:
                secs = max(2, min(30, int(args.get("seconds") or 6)))
            except (TypeError, ValueError):
                secs = 6
            log("voice: muse_show %r for %d s" % (text, secs))
            if text:
                self.link.queue_voice("json", {"type": "voice", "show": text, "secs": secs})
            content = json.dumps({"ok": bool(text), "showing": text})
        elif name == "muse_set":
            chosen = {}
            for key, low in (("brightness", 10), ("volume", 0)):
                try:
                    if args.get(key) is not None:
                        chosen[key] = max(low, min(100, int(args[key])))
                except (TypeError, ValueError):
                    pass
            log("voice: muse_set", chosen)
            if chosen:
                self.link.send_json({"type": "voice", "set": chosen})
            content = json.dumps({"ok": bool(chosen), **chosen})
        elif name == "muse_status":
            content = json.dumps(self.link.status_report())
            log("voice: muse_status ->", content)
        elif name == "time_now":
            content = json.dumps(muse_world.time_report(options()["timezone"]))
        elif name == "timer":
            timers, action = self.link.hub.timers, args.get("action")
            if action == "set":
                result = timers.set(args.get("minutes"), args.get("label"))
            elif action == "cancel":
                result = timers.cancel(args.get("label"))
            else:
                result = timers.list()
            log("voice: timer", action, "->", result)
            content = json.dumps(result)
        elif name == "remember":
            result = muse_world.remember(args.get("note") or "")
            log("voice: remember ->", result)
            content = json.dumps(result)
        elif name == "forget":
            result = muse_world.forget(args.get("about") or "")
            log("voice: forget ->", result)
            content = json.dumps(result)
        elif name == "work_on":
            result = self.link.hub.jobs.start(args.get("task"), args.get("title"), options()["worker_model"])
            log("voice: work_on ->", result)
            content = json.dumps(result)
        elif name == "work_status":
            content = json.dumps(self.link.hub.jobs.for_ai())
            log("voice: work_status ->", content[:200])
        elif name == "claude_status":
            content = self.claude_status()
            log("voice: claude_status ->", content)
        else:
            content = json.dumps({"error": "unknown function"})
        self._send_json({"type": "FunctionCallResponse", "id": f.get("id"), "name": name, "content": content})
