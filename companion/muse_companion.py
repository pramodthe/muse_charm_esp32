#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Muse companion: Muse shows what Claude Code is doing on this computer.

Claude Code hooks send each event to http://127.0.0.1:8765/hook (see
install_hooks.py). This server turns the events into a mood and a short
caption, and passes them to Muse over a websocket. Muse finds the server by
itself: the server announces itself on the local network with a small UDP
beacon, and Muse connects to ws://<this computer>:8765/device.

    python3 muse_companion.py          run the server
    python3 muse_companion.py demo     play a made-up Claude Code session
                                       through a running server
    curl http://127.0.0.1:8765/        what the server sees right now

Only the Python standard library is used (Python 3.8 or later).

Privacy: prompts, file contents and commands stay on this computer. Muse only
receives the mood, the tool's short description (for example "Editing
face.c" or the one-line description Claude gives a shell command) and the
project folder name.
"""

import argparse
import asyncio
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time
import urllib.request

PORT = 8765
BEACON_PORT = 47800
BEACON_EVERY_S = 2.0
SESSION_STALE_S = 30 * 60   # forget a session that has gone quiet this long
THINKING_STALE_S = 5 * 60   # "thinking" with no news this long: Claude was interrupted
OOPS_EVERY_S = 10           # at most one "that failed" reaction per session this often
CAPTION_MAX = 28            # characters that fit under Muse on one line
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_HOOK_BYTES = 32 * 1024 * 1024

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
    def __init__(self, name):
        self.name = name
        self.sessions = {}
        self.devices = {}     # writer -> device name
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
        data = frame(0x1, json.dumps(msg).encode())
        for w in list(self.devices):
            try:
                w.write(data)
            except Exception:
                self.devices.pop(w, None)

    # ------------------------------------------------------------ devices
    async def serve_device(self, reader, writer, peer):
        self.devices[writer] = peer
        log("Muse connected from", peer)
        writer.write(frame(0x1, json.dumps({"type": "hello", "name": self.name}).encode()))
        if self.sent:
            writer.write(frame(0x1, json.dumps(self.sent).encode()))
        try:
            while True:
                op, data = await asyncio.wait_for(read_frame(reader), 60)
                if op == 0x8:       # close
                    writer.write(frame(0x8, data[:2]))
                    break
                if op == 0x9:       # ping
                    writer.write(frame(0xA, data))
                elif op == 0x1:
                    try:
                        info = json.loads(data)
                        log("Muse says", info)
                    except ValueError:
                        pass
                await writer.drain()
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError, OSError):
            pass
        finally:
            self.devices.pop(writer, None)
            log("Muse disconnected", peer)
            writer.close()

    def status(self):
        return {
            "server": self.name,
            "devices": list(self.devices.values()),
            "showing": self.sent,
            "sessions": [
                {"project": s.project, "mood": s.mood, "caption": s.caption,
                 "idle_s": int(time.time() - s.updated)}
                for s in self.sessions.values()
            ],
        }


# ---------------------------------------------------------------- websocket frames
def frame(op, payload):
    n = len(payload)
    if n < 126:
        head = struct.pack("!BB", 0x80 | op, n)
    elif n < 65536:
        head = struct.pack("!BBH", 0x80 | op, 126, n)
    else:
        head = struct.pack("!BBQ", 0x80 | op, 127, n)
    return head + payload


async def read_frame(reader):
    b1, b2 = await reader.readexactly(2)
    n = b2 & 0x7F
    if n == 126:
        (n,) = struct.unpack("!H", await reader.readexactly(2))
    elif n == 127:
        (n,) = struct.unpack("!Q", await reader.readexactly(8))
    if n > 1 << 20:
        raise ConnectionError("frame too large")
    mask = await reader.readexactly(4) if b2 & 0x80 else None
    data = await reader.readexactly(n)
    if mask:
        data = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
    return b1 & 0x0F, data


# ---------------------------------------------------------------- HTTP
def respond(writer, status, body=b"", ctype="application/json"):
    writer.write(("HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
                  % (status, ctype, len(body))).encode() + body)


def is_loopback(host):
    return host in ("127.0.0.1", "::1", "::ffff:127.0.0.1")


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

        if method == "POST" and path == "/hook":
            if not is_loopback(peer):
                respond(writer, "403 Forbidden")       # hooks come from this computer only
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

        if path == "/device" and headers.get("upgrade", "").lower() == "websocket":
            key = headers.get("sec-websocket-key", "")
            accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
            writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
            await writer.drain()
            await hub.serve_device(reader, writer, peer)
            return

        if method == "GET" and path == "/":
            respond(writer, "200 OK", json.dumps(hub.status(), indent=2).encode() + b"\n")
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
    while True:
        await asyncio.sleep(20)
        for w in list(hub.devices):
            try:
                w.write(frame(0x9, b""))
            except Exception:
                hub.devices.pop(w, None)
        s = hub.focus()
        now = (s.mood, s.caption) if s else ("idle", "")
        if hub.sent and (hub.sent.get("mood"), hub.sent.get("caption")) != now:
            hub.publish()  # a session went quiet: let Muse rest


def computer_name():
    name = socket.gethostname().split(".")[0]
    return ascii_only(name.replace("-", " "))[:20] or "this computer"


async def serve(port):
    hub = Hub(computer_name())
    server = await asyncio.start_server(lambda r, w: handle(hub, r, w), host="0.0.0.0", port=port,
                                        limit=1 << 16)
    log("Muse companion on port %d (%s, %s)" % (port, hub.name, local_ip() or "no network"))
    log("waiting for Muse and for Claude Code hooks; Ctrl-C to stop")
    tasks = [asyncio.ensure_future(beacon(hub, port)), asyncio.ensure_future(keepalive(hub))]  # keep references
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


def demo(port):
    url = "http://127.0.0.1:%d/hook" % port
    for delay, ev in DEMO:
        time.sleep(delay)
        ev = dict(ev, session_id="demo", cwd="/demo/muse_charm_esp32")
        print("->", ev["hook_event_name"], ev.get("tool_name", ""), flush=True)
        req = urllib.request.Request(url, data=json.dumps(ev).encode(),
                                     headers={"Content-Type": "application/json"})
        try:
            urllib.request.urlopen(req, timeout=2).read()
        except OSError as e:
            sys.exit("the server is not running (%s); start it with: python3 muse_companion.py" % e)


def main():
    ap = argparse.ArgumentParser(description="Muse companion server for Claude Code")
    ap.add_argument("command", nargs="?", default="serve", choices=["serve", "demo"])
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()
    if args.command == "demo":
        demo(args.port)
        return
    try:
        asyncio.run(serve(args.port))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
