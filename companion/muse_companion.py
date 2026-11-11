#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Muse companion: Muse shows what Claude Code is doing on this computer, and
you can talk to Muse.

Claude Code hooks send each event to http://127.0.0.1:8765/hook (see
install_hooks.py). This server turns the events into a mood and a short
caption, and passes them to Muse over a websocket. Muse finds the server by
itself: the server announces itself on the local network with a small UDP
beacon, and Muse connects to ws://<this computer>:8765/device.

Voice chat goes through the same connection: Muse streams its microphone here
and this server talks to the Deepgram Voice Agent API (muse_voice.py), so the
Deepgram key stays on this computer (in the repository's .env file).

    python3 muse_companion.py          run the server
    python3 muse_companion.py demo     play a made-up Claude Code session
                                       through a running server
    python3 muse_companion.py pair     run the server and tell the Muse that
                                       connects where the online server is
    python3 muse_companion.py results  open the page with what Muse's
                                       background jobs wrote (muse_jobs.py)
    curl http://127.0.0.1:8765/        what the server sees right now

Online: the same server can run on a host on the internet, so Muse works
without this computer (see deploy_modal.py and the README). Setting MUSE_TOKEN
puts it in online mode: Muse and the hooks must then present that token, and
the local-network beacon is off.

Only the Python standard library is used (Python 3.8 or later).

Privacy: prompts, file contents and commands stay on this computer. Muse only
receives the mood, the tool's short description (for example "Editing
face.c" or the one-line description Claude gives a shell command) and the
project folder name.
"""

import argparse
import asyncio
import hmac
import json
import os
import socket
import sys
import time
import urllib.parse
import urllib.request

import muse_jobs
import muse_online
import muse_results
import muse_voice
import muse_world
import muse_ws as ws

PORT = 8765
BEACON_PORT = 47800
BEACON_EVERY_S = 2.0
SESSION_STALE_S = 30 * 60   # forget a session that has gone quiet this long
THINKING_STALE_S = 5 * 60   # "thinking" with no news this long: Claude was interrupted
OOPS_EVERY_S = 10           # at most one "that failed" reaction per session this often
CAPTION_MAX = 28            # characters that fit under Muse on one line
MAX_HOOK_BYTES = 32 * 1024 * 1024
RETIRE_S = 300              # online: how long a retiring server turns Muse away (a deploy can take minutes)

# Moods last until the next event; events are one-off reactions.
MOOD_PRIORITY = {"needs_you": 3, "working": 2, "thinking": 1, "idle": 0}


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


def ascii_only(text):
    # Muse's built-in font has ASCII only.
    text = text.replace("…", "...").replace("’", "'").replace("—", "-")
    return "".join(c if 32 <= ord(c) < 127 else "" for c in text).strip()


def short(text, limit=CAPTION_MAX):
    text = ascii_only(" ".join(str(text).split()))
    return text if len(text) <= limit else text[: limit - 3].rstrip() + "..."


def basename(path):
    return os.path.basename(str(path or "").rstrip("/")) or "a file"


def approval_caption(tool, tool_input):
    """What Claude asks permission for, as a question."""
    ti = tool_input if isinstance(tool_input, dict) else {}
    what = None
    if tool == "Bash" and ti.get("description"):
        d = ascii_only(str(ti["description"]))
        what = d[:1].lower() + d[1:]
    elif tool in ("Edit", "MultiEdit", "Write"):
        what = "edit " + basename(ti.get("file_path"))
    elif tool == "WebFetch":
        host = str(ti.get("url") or "").split("/")[2:3]
        what = "open " + host[0] if host else None
    if not what:
        return "Needs your OK"
    return short("OK to " + what.rstrip(".") + "?")


def tool_caption(tool, tool_input):
    """A short description of a tool call, without its contents."""
    ti = tool_input if isinstance(tool_input, dict) else {}
    if tool == "Bash":
        return short(ti.get("description") or "Running a command")
    if tool == "Read":
        return short("Reading " + basename(ti.get("file_path")))
    if tool in ("Edit", "MultiEdit", "Write"):
        return short("Editing " + basename(ti.get("file_path")))
    if tool == "NotebookEdit":
        return short("Editing " + basename(ti.get("notebook_path")))
    if tool in ("Grep", "Glob", "LS"):
        return "Looking through files"
    if tool == "WebSearch":
        return "Searching the web"
    if tool == "WebFetch":
        host = str(ti.get("url") or "").split("/")[2:3]
        return short("Reading " + (host[0] if host else "the web"))
    if tool in ("Task", "Agent"):
        return "Asking a helper"
    if tool in ("TodoWrite", "TaskCreate", "TaskUpdate"):
        return "Planning"
    if tool.startswith("mcp__"):
        parts = tool.split("__")
        return short("Using " + (parts[1] if len(parts) > 1 else "a tool").replace("_", " "))
    return short("Using " + (tool or "a tool"))


class Session:
    def __init__(self, sid, project):
        self.sid = sid
        self.project = project
        self.mood = "idle"
        self.caption = ""
        self.running = {}     # tool_use_id -> caption, for parallel tool calls
        self.updated = time.time()
        self.last_oops = 0.0


class Hub:
    def __init__(self, name, token="", pair=None):
        self.name = name
        self.token = token    # online mode: what Muse and the hooks must present
        self.pair = pair      # {"url", "token"} to hand to the Muse that connects, or None
        self.sessions = {}
        self.devices = {}     # writer -> DeviceLink
        self.timers = muse_world.Timers(self.alert)
        self.missed = []      # alerts that came up while no Muse was connected
        self.battery_told = 100   # the battery level Muse last warned about (100: not yet)
        self.jobs = muse_jobs.Jobs(self.on_job_done)   # background work (muse_worker.py)
        self.retired_until = 0.0  # online: a new version is taking over; turn devices away until then
        # Online: let anyone who has the address read the results page without the token.
        self.results_public = os.environ.get("MUSE_RESULTS_PUBLIC", "").strip().lower() in ("1", "true", "yes", "on")
        self.sent = None      # last mood message, re-sent to new devices

    # ------------------------------------------------------------ hook events
    def on_hook(self, ev):
        name = ev.get("hook_event_name", "")
        sid = str(ev.get("session_id") or "?")
        project = short(basename(ev.get("cwd")) if ev.get("cwd") else "", 20)
        s = self.sessions.get(sid)
        if s is None:
            if name == "SessionEnd":
                return
            s = self.sessions[sid] = Session(sid, project)
        s.updated = time.time()
        tool = ev.get("tool_name") or ""
        use_id = ev.get("tool_use_id") or tool
        event, event_caption = None, ""

        if name == "SessionStart":
            if ev.get("source") == "compact":
                return  # the same session carries on after compaction
            if ev.get("source") == "clear":
                s.mood, s.caption, s.running = "idle", "", {}
            else:
                s.mood, s.caption, s.running = "idle", "", {}
                event, event_caption = "hello", short("Hi! " + (s.project or "Let's go"))
        elif name == "UserPromptSubmit":
            s.mood, s.caption, s.running = "thinking", "Thinking...", {}
        elif name == "PreToolUse":
            s.running[use_id] = tool_caption(tool, ev.get("tool_input"))
            s.mood, s.caption = "working", s.running[use_id]
        elif name in ("PostToolUse", "PostToolUseFailure"):
            s.running.pop(use_id, None)
            if s.running:
                s.mood, s.caption = "working", list(s.running.values())[-1]
            else:
                s.mood, s.caption = "thinking", "Thinking..."
            if name == "PostToolUseFailure" and not ev.get("is_interrupt"):
                if time.time() - s.last_oops > OOPS_EVERY_S:
                    s.last_oops = time.time()
                    event, event_caption = "oops", "Hmm, that didn't work"
        elif name == "PermissionRequest":
            s.mood, s.caption = "needs_you", approval_caption(tool, ev.get("tool_input"))
        elif name == "Notification":
            kind = ev.get("notification_type") or ""
            if kind in ("permission_prompt", "elicitation_dialog", "elicitation_url_dialog"):
                if s.mood != "needs_you":
                    s.mood, s.caption = "needs_you", "Needs your OK"
            else:
                return  # idle reminders and the like: Muse already cheered at Stop
        elif name == "SubagentStart":
            s.running["agent:" + str(ev.get("agent_id"))] = "Asking a helper"
            s.mood, s.caption = "working", "Asking a helper"
        elif name == "SubagentStop":
            s.running.pop("agent:" + str(ev.get("agent_id")), None)
            if not s.running:
                s.mood, s.caption = "thinking", "Thinking..."
        elif name == "Stop":
            s.mood, s.caption, s.running = "idle", "", {}
            event, event_caption = "done", "Done!"
        elif name == "StopFailure":
            s.mood, s.caption, s.running = "idle", "", {}
            event, event_caption = "error", "Something went wrong"
        elif name == "PreCompact":
            event, event_caption = "compact", "Tidying up memory"
        elif name == "SessionEnd":
            self.sessions.pop(sid, None)
            event, event_caption = "bye", "Bye!"
        else:
            return

        log("%-18s %-9s %s" % (name, s.mood, event_caption or s.caption), "[%s]" % (s.project or sid[:8]))
        self.publish(event, event_caption)

    def focus(self):
        """The session Muse should show: the one that most needs attention."""
        now = time.time()
        for sid in [k for k, s in self.sessions.items() if now - s.updated > SESSION_STALE_S]:
            del self.sessions[sid]
        for s in self.sessions.values():
            if s.mood == "thinking" and now - s.updated > THINKING_STALE_S:
                s.mood, s.caption = "idle", ""
        best = None
        for s in self.sessions.values():
            key = (MOOD_PRIORITY.get(s.mood, 0), s.updated)
            if best is None or key > best[0]:
                best = (key, s)
        return best[1] if best else None

    def publish(self, event=None, event_caption=""):
        s = self.focus()
        msg = {"type": "claude", "mood": s.mood if s else "idle", "caption": s.caption if s else ""}
        if event:
            msg["event"] = event
            msg["event_caption"] = event_caption
        self.sent = {"type": "claude", "mood": msg["mood"], "caption": msg["caption"]}
        self.broadcast(msg)

    def broadcast(self, msg):
        for link in list(self.devices.values()):
            link.send_json(msg)

    def alert(self, text):
        """A timer is up: Muse says so, waves and shows its label (when Muse
        cannot speak: chirps, a wave and the label)."""
        log("alert:", text)
        label = short(text, 18)
        for link in list(self.devices.values()):
            link.voice.announce(text=muse_world.timer_line(text), moves=["wave"], sign=label, secs=15, alert=label)
        if not self.devices:
            self.missed = (self.missed + [text])[-3:]

    def start_job(self, task, title=""):
        """Hand a job to the background worker, with what Muse knows about you."""
        opts = muse_voice.options()
        t = muse_world.time_report(opts["timezone"])
        return self.jobs.start(task, title, opts["worker_model"], about=muse_world.notes(),
                               today="%s %s" % (t["weekday"], t["date"]))

    def app_state(self):
        """For the web app: how Muse is, its timers, and what it remembers."""
        return {
            "connected": len(self.devices),
            "muse": next((link.status_report() for link in self.devices.values() if link.status), None),
            "timers": self.timers.list()["timers"],
            "memory": muse_world.notes(),
            "voice": muse_voice.key_status()[0] is not None,
            "cannot_work": self.jobs.problem() or "",
        }

    def on_job_done(self, job):
        """A background job has finished: Muse says so, with the answer in a sentence."""
        if job.get("status") == "done":
            line = "I'm done! " + (job.get("summary") or "The write-up on %s is on your results page." % job["title"])
            sign, moves = "Ready!", ["cheer"]
        else:
            line, sign, moves = "Sorry, I couldn't finish looking into %s." % job["title"], "Oops", ["shake_head"]
        for link in list(self.devices.values()):
            link.voice.announce(text=line, moves=moves, sign=sign, secs=15, alert=sign)
        if not self.devices:
            self.missed = (self.missed + [sign])[-3:]

    def on_status(self, link):
        """Muse reported how it is doing: say so when its battery runs low
        (once at low and once when nearly empty, and not at night)."""
        percent = link.status.get("battery")
        if percent is None:
            return
        if link.status.get("charging") or link.status.get("usb") or percent >= muse_world.BATTERY_FINE:
            self.battery_told = 100
            return
        warning = muse_world.battery_line(percent, self.battery_told)
        if warning and muse_voice.options()["speak_first"] and muse_world.daytime(muse_voice.options()["timezone"]):
            self.battery_told, line, sign = warning
            log("battery at %d %%: telling" % percent)
            link.voice.announce(text=line, moves=["tilt_head"], sign=sign, secs=20, alert=sign)

    def on_awake(self, link):
        """Muse was picked up or woken: the first time in a morning, it says hello."""
        opts = muse_voice.options()
        if opts["speak_first"] and muse_voice.read_key() and muse_world.greeting_due(opts["timezone"]):
            log("good morning")
            link.voice.announce(note=muse_world.GREETING_NOTE)

    def claude_summary(self):
        """What Claude Code is up to, for Muse to talk about."""
        now = time.time()
        self.focus()  # drops stale sessions
        words = {"needs_you": "waiting for the user's permission", "working": "running a tool",
                 "thinking": "thinking", "idle": "idle, waiting for the user"}
        sessions = [{"project": s.project or "unknown", "state": words.get(s.mood, s.mood),
                     "doing": s.caption, "minutes_since_last_activity": int((now - s.updated) // 60)}
                    for s in self.sessions.values()]
        if not sessions:
            return json.dumps({"sessions": [], "note": "No Claude Code sessions are running right now."})
        return json.dumps({"sessions": sessions})

    # ------------------------------------------------------------ devices
    async def serve_device(self, reader, writer, peer):
        link = DeviceLink(self, writer, peer)
        self.devices[writer] = link
        log("Muse connected from", peer)
        link.send_json({"type": "hello", "name": self.name})
        if self.pair is not None:
            link.send_json(dict(self.pair, type="server"))
        link.announce_voice()
        if self.sent:
            link.send_json(self.sent)
        for text in self.missed:
            link.send_json({"type": "alert", "text": short(text, 18)})
        self.missed = []
        try:
            while True:
                op, data = await ws.read_message(reader, writer, frame_timeout=60)  # Muse pings every 10 s
                if op == ws.CLOSE:
                    writer.write(ws.frame(ws.CLOSE, data[:2]))
                    break
                if op == ws.BINARY:
                    link.voice.mic(data)
                elif op == ws.TEXT:
                    link.on_text(data)
                await writer.drain()
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            self.devices.pop(writer, None)
            link.close()
            log("Muse disconnected", peer)
            writer.close()

    def status(self):
        return {
            "server": self.name,
            "devices": [link.peer for link in self.devices.values()],
            "muse": [link.status_report() for link in self.devices.values() if link.status],
            "timers": self.timers.list()["timers"],
            "jobs": [self.jobs.brief(j) for j in self.jobs.list()[:5]],
            "voice": muse_voice.key_status()[1] or "ready",
            "showing": self.sent,
            "sessions": [
                {"project": s.project, "mood": s.mood, "caption": s.caption,
                 "idle_s": int(time.time() - s.updated)}
                for s in self.sessions.values()
            ],
        }


class DeviceLink:
    """One connected Muse: its websocket, and its voice conversation."""

    def __init__(self, hub, writer, peer):
        self.hub, self.writer, self.peer = hub, writer, peer
        self.voice = muse_voice.Voice(self, hub.claude_summary)
        self.voice_queue = asyncio.Queue()   # reply audio and voice messages, in order
        self.voice_on = None
        self.status, self.status_at = {}, 0.0   # what Muse last reported about itself
        self.sender = asyncio.ensure_future(self._send_voice())

    def send_json(self, msg):
        try:
            self.writer.write(ws.frame(ws.TEXT, json.dumps(msg).encode()))
        except Exception:
            pass

    def announce_voice(self):
        on = muse_voice.read_key() is not None
        if on != self.voice_on:
            self.voice_on = on
            self.send_json({"type": "voice_ready", "on": on})

    def on_text(self, data):
        try:
            msg = json.loads(data)
        except ValueError:
            return
        t = msg.get("type")
        if t == "talk_start":
            self.voice.talk_start()
        elif t == "talk_end":
            self.voice.talk_end()
        elif t == "talk_stop":
            self.voice.talk_stop()
        elif t == "status":
            self.status, self.status_at = {k: v for k, v in msg.items() if k != "type"}, time.time()
            self.hub.on_status(self)
        elif t == "awake":
            self.hub.on_awake(self)
        elif t == "hello":
            log("Muse says hello", {k: v for k, v in msg.items() if k != "type"})
        elif t == "paired":
            if msg.get("url"):
                log("Muse saved the online server. Stop this server (Ctrl-C) and Muse will use it.")
            else:
                log("Muse forgot the online server; it will only look for one on this network.")

    def status_report(self):
        # A connection that reports nothing about itself (test_voice.py) gets the
        # newest report from a Muse that does.
        link = self if self.status else max(self.hub.devices.values(), key=lambda d: d.status_at, default=self)
        return muse_world.status_report(link.status, time.time() - link.status_at)

    # Reply audio is paced: at most LEAD_S ahead of playback, so Muse's buffer
    # never overflows and a "stop" takes effect quickly.
    def queue_voice(self, kind, data):
        self.voice_queue.put_nowait((kind, data))

    def flush_voice(self):
        while not self.voice_queue.empty():
            self.voice_queue.get_nowait()

    async def _send_voice(self):
        start, sent = time.monotonic(), 0.0
        try:
            while True:
                kind, data = await self.voice_queue.get()
                if kind == "start":
                    start, sent = time.monotonic(), 0.0
                    self.send_json({"type": "voice", "state": "speaking"})
                elif kind == "json":
                    self.send_json(data)
                elif kind == "audio":
                    ahead = sent - (time.monotonic() - start)
                    if ahead < 0:
                        start, sent = time.monotonic(), 0.0   # fell behind: restart the clock
                    elif ahead > muse_voice.LEAD_S:
                        await asyncio.sleep(ahead - muse_voice.LEAD_S)
                    self.writer.write(ws.frame(ws.BINARY, data))
                    sent += len(data) / muse_voice.BYTES_PER_S
                    await self.writer.drain()
        except (asyncio.CancelledError, ConnectionError, OSError):
            pass

    def close(self):
        self.sender.cancel()
        self.voice.close()


# ---------------------------------------------------------------- HTTP
def respond(writer, status, body=b"", ctype="application/json"):
    writer.write(("HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
                  % (status, ctype, len(body))).encode() + body)


def is_loopback(host):
    return host in ("127.0.0.1", "::1", "::ffff:127.0.0.1")


def authorized(hub, headers):
    """Online mode: the request carries the token."""
    given = headers.get("authorization", "")
    if given.lower().startswith("bearer "):
        given = given[7:].strip()
    return bool(hub.token) and hmac.compare_digest(given.encode(), hub.token.encode())


async def handle(hub, reader, writer):
    peer = (writer.get_extra_info("peername") or ("?",))[0]
    try:
        head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
        lines = head.decode("latin-1").split("\r\n")
        method, target = lines[0].split(" ")[:2]
        path = target.split("?")[0]
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()

        if hub.token:
            peer = headers.get("x-forwarded-for", peer).split(",")[0].strip()  # behind the host's proxy

        if method == "POST" and path == "/hook":
            # Hooks come from this computer only; online, from whoever has the token.
            if not (authorized(hub, headers) if hub.token else is_loopback(peer)):
                respond(writer, "403 Forbidden")
                return
            n = int(headers.get("content-length") or 0)
            if n > MAX_HOOK_BYTES:
                respond(writer, "413 Payload Too Large")
                return
            if headers.get("expect", "").lower() == "100-continue":
                writer.write(b"HTTP/1.1 100 Continue\r\n\r\n")
            body = await asyncio.wait_for(reader.readexactly(n), 10)
            try:
                ev = json.loads(body)
            except ValueError:
                respond(writer, "400 Bad Request")
                return
            respond(writer, "204 No Content")
            await writer.drain()
            if isinstance(ev, dict):
                hub.on_hook(ev)
            return

        if method == "POST" and path == "/retire" and hub.token:
            # A new version is about to be deployed. A host such as Modal keeps this
            # one running for as long as Muse stays connected to it, so let Muse go
            # and turn it away for a while; it keeps trying and lands on the new
            # version. Only for a while, in case the new version never arrives.
            if not authorized(hub, headers):
                respond(writer, "403 Forbidden")
                return
            log("retiring: letting %d device(s) go" % len(hub.devices))
            hub.retired_until = time.time() + RETIRE_S
            for link in list(hub.devices.values()):
                try:
                    link.writer.write(ws.frame(ws.CLOSE, b"\x03\xe9"))  # 1001 going away
                    link.writer.close()
                except Exception:
                    pass
            respond(writer, "204 No Content")
            return

        if path == "/device" and headers.get("upgrade", "").lower() == "websocket":
            if hub.token and not authorized(hub, headers):
                log("refused a device without the token from", peer)
                respond(writer, "401 Unauthorized")
                return
            if time.time() < hub.retired_until:
                respond(writer, "503 Service Unavailable")
                return
            key = headers.get("sec-websocket-key", "")
            writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n" % ws.accept_key(key)).encode())
            await writer.drain()
            await hub.serve_device(reader, writer, peer)
            return

        if path in ("/app.json", "/ask", "/memory"):
            # The web app's private side: Muse's state, new requests, its memory.
            if not (authorized(hub, headers) if hub.token else (method == "GET" or is_loopback(peer))):
                respond(writer, "401 Unauthorized")
                return
            if method == "GET" and path == "/app.json":
                respond(writer, "200 OK", json.dumps(hub.app_state()).encode())
                return
            # Only the page itself may post: a form on another site cannot send these headers.
            n = int(headers.get("content-length") or 0)
            if (method != "POST" or path == "/app.json" or n > 16384 or "x-muse" not in headers
                    or not headers.get("content-type", "").startswith("application/json")):
                respond(writer, "400 Bad Request")
                return
            try:
                body = json.loads(await asyncio.wait_for(reader.readexactly(n), 10))
                if not isinstance(body, dict):
                    raise ValueError
            except ValueError:
                respond(writer, "400 Bad Request")
                return
            if path == "/ask":
                result = hub.start_job(body.get("task"), body.get("title") or "")
                log("asked from the web app ->", result)
            elif body.get("forget"):
                result = muse_world.forget_note(body["forget"])
            else:
                result = muse_world.remember(str(body.get("add") or ""))
            respond(writer, "200 OK", json.dumps(result).encode())
            return

        if method == "GET" and path == "/results":
            respond(writer, "200 OK", muse_results.PAGE.encode(), "text/html; charset=utf-8")
            return
        if method == "GET" and path.startswith("/results") and path.endswith(".json"):
            if hub.token and not hub.results_public and not authorized(hub, headers):
                respond(writer, "401 Unauthorized")
                return
            if path == "/results.json":
                found = {"jobs": [hub.jobs.brief(j) for j in hub.jobs.list()]}
            else:
                found = hub.jobs.get(path[len("/results/"):-len(".json")])
            if found is None:
                respond(writer, "404 Not Found")
            else:
                respond(writer, "200 OK", json.dumps(found).encode())
            return

        if method == "GET" and path == "/":
            status = hub.status() if not hub.token or authorized(hub, headers) else {"server": "muse companion"}
            respond(writer, "200 OK", json.dumps(status, indent=2).encode() + b"\n")
            return
        respond(writer, "404 Not Found")
    except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError,
            ConnectionError, OSError, ValueError):
        pass
    finally:
        try:
            await writer.drain()
            writer.close()
        except Exception:
            pass


# ---------------------------------------------------------------- beacon
def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))   # no packet is sent; this picks the LAN interface
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


async def beacon(hub, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    msg = ("MUSE-COMPANION 1 %d %s" % (port, hub.name)).encode()
    warned = False
    while True:
        targets = {"255.255.255.255"}
        ip = local_ip()
        if ip:
            targets.add(ip.rsplit(".", 1)[0] + ".255")
        sent = False
        for t in targets:
            try:
                sock.sendto(msg, (t, BEACON_PORT))
                sent = True
            except OSError:
                pass
        if not sent and not warned:
            log("could not send the beacon; is this computer on WiFi?")
        warned = not sent
        await asyncio.sleep(BEACON_EVERY_S)


async def keepalive(hub):
    n = 0
    while True:
        await asyncio.sleep(5)
        n += 1
        for link in list(hub.devices.values()):
            link.announce_voice()           # the key file may have appeared
            if n % 4 == 0:
                try:
                    link.writer.write(ws.frame(ws.PING, b""))
                except Exception:
                    pass
        s = hub.focus()
        now = (s.mood, s.caption) if s else ("idle", "")
        if hub.sent and (hub.sent.get("mood"), hub.sent.get("caption")) != now:
            hub.publish()  # a session went quiet: let Muse rest


def computer_name():
    name = socket.gethostname().split(".")[0]
    return ascii_only(name.replace("-", " "))[:20] or "this computer"


async def serve(port, pair=None):
    token = os.environ.get("MUSE_TOKEN", "").strip()
    name = os.environ.get("MUSE_NAME", "the online server") if token else computer_name()
    hub = Hub(name, token, pair)
    server = await asyncio.start_server(lambda r, w: handle(hub, r, w), host="0.0.0.0", port=port,
                                        limit=1 << 16)
    if token:
        log("Muse companion online on port %d; Muse and the hooks need the token" % port)
    else:
        log("Muse companion on port %d (%s, %s)" % (port, hub.name, local_ip() or "no network"))
    log("voice chat:", muse_voice.key_status()[1] or "ready")
    if pair is not None:
        log("pairing: the Muse that connects will be told %s" % (
            "to use " + pair["url"] if pair["url"] else "to forget the online server"))
    log("waiting for Muse and for Claude Code hooks; Ctrl-C to stop")
    tasks = [asyncio.ensure_future(keepalive(hub))]  # keep references
    if not token:
        tasks.append(asyncio.ensure_future(beacon(hub, port)))  # Muse finds a local server by itself
    async with server:
        await server.serve_forever()


# ---------------------------------------------------------------- demo
DEMO = [
    (0.0, {"hook_event_name": "SessionStart", "source": "startup"}),
    (3.5, {"hook_event_name": "UserPromptSubmit", "prompt": "(demo)"}),
    (2.5, {"hook_event_name": "PreToolUse", "tool_name": "Read", "tool_use_id": "1",
           "tool_input": {"file_path": "/demo/firmware/main/face.c"}}),
    (2.0, {"hook_event_name": "PostToolUse", "tool_name": "Read", "tool_use_id": "1"}),
    (2.0, {"hook_event_name": "PreToolUse", "tool_name": "Edit", "tool_use_id": "2",
           "tool_input": {"file_path": "/demo/firmware/main/face.c"}}),
    (2.5, {"hook_event_name": "PostToolUse", "tool_name": "Edit", "tool_use_id": "2"}),
    (1.5, {"hook_event_name": "PreToolUse", "tool_name": "Bash", "tool_use_id": "3",
           "tool_input": {"description": "Run the tests"}}),
    (3.0, {"hook_event_name": "PostToolUseFailure", "tool_name": "Bash", "tool_use_id": "3",
           "error": "Exit code 1"}),
    (3.0, {"hook_event_name": "PreToolUse", "tool_name": "Bash", "tool_use_id": "4",
           "tool_input": {"description": "Flash the firmware"}}),
    (0.5, {"hook_event_name": "PermissionRequest", "tool_name": "Bash",
           "tool_input": {"description": "Flash the firmware"}}),
    (8.0, {"hook_event_name": "PostToolUse", "tool_name": "Bash", "tool_use_id": "4"}),
    (2.0, {"hook_event_name": "PreCompact"}),
    (3.0, {"hook_event_name": "Stop"}),
    (8.0, {"hook_event_name": "SessionEnd", "reason": "other"}),
]


def demo(port, online):
    url, headers = "http://127.0.0.1:%d/hook" % port, {"Content-Type": "application/json"}
    if online:
        saved = muse_online.load()
        if not saved.get("url"):
            sys.exit("no online server saved in %s" % muse_online.SERVER_FILE)
        url = muse_online.http_url(saved["url"], "/hook")
        headers["Authorization"] = "Bearer " + saved.get("token", "")
    for delay, ev in DEMO:
        time.sleep(delay)
        ev = dict(ev, session_id="demo", cwd="/demo/muse_charm_esp32")
        print("->", ev["hook_event_name"], ev.get("tool_name", ""), flush=True)
        req = urllib.request.Request(url, data=json.dumps(ev).encode(), headers=headers)
        try:
            urllib.request.urlopen(req, timeout=10 if online else 2).read()
        except OSError as e:
            sys.exit("the server did not take it (%s)%s" % (
                e, "" if online else "; start it with: python3 muse_companion.py"))


def open_results(port):
    """Open the page with what Muse's background jobs wrote, signed in."""
    import webbrowser
    saved = muse_online.load()
    if saved.get("url"):
        page = muse_online.http_url(saved["url"], "/results")
        # After the "#": browsers keep that part to themselves, and the page stores it.
        webbrowser.open(page + "#" + urllib.parse.quote(saved.get("token", ""), safe=""))
    else:
        page = "http://127.0.0.1:%d/results" % port
        webbrowser.open(page)
    print("Opened %s in your browser. It stays signed in there; bookmark it." % page)


def main():
    ap = argparse.ArgumentParser(description="Muse companion server for Claude Code and voice chat")
    ap.add_argument("command", nargs="?", default="serve", choices=["serve", "demo", "pair", "results"])
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT") or PORT))
    ap.add_argument("--online", action="store_true", help="demo: play it through the online server")
    ap.add_argument("--forget", action="store_true", help="pair: make Muse forget the online server")
    args = ap.parse_args()
    if args.command == "demo":
        demo(args.port, args.online)
        return
    if args.command == "results":
        open_results(args.port)
        return
    pair = None
    if args.command == "pair":
        saved = muse_online.load()
        if args.forget:
            pair = {"url": "", "token": ""}
        elif saved.get("url") and saved.get("token"):
            pair = {"url": muse_online.device_url(saved["url"]), "token": saved["token"]}
        else:
            sys.exit("no online server saved in %s; deploy one first (see the README)" % muse_online.SERVER_FILE)
    try:
        asyncio.run(serve(args.port, pair))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
