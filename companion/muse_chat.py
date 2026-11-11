# SPDX-License-Identifier: MIT
"""One conversation with Muse, whether you talk to it or type to it.

Conversation keeps what was said, by you and by Muse, out loud and in the web
app, in chat.json in ~/.config/muse (or MUSE_DATA_DIR). The web app shows it as
one chat; both the voice (muse_voice.py) and the typed chat below are given the
recent part of it, so each knows what happened in the other.

    {"t": 1794400000.0, "who": "you" | "muse", "via": "voice" | "web",
     "text": "..."}            something said or typed
    {"t": ..., "who": "muse", "job": "<id>"}   a background job was started

reply() answers a typed message. It asks Claude through the Anthropic API
(with the same key as the background jobs), with the same tools the voice has
(muse_voice.TOOLS, carried out by muse_voice.run_tool), so a typed "dance for
me" makes the Muse on your desk dance. Only the standard library is used.

MUSE_CHAT_FAKE=1 answers without a model (for testing).
"""

import asyncio
import json
import os
import time
import urllib.error
import urllib.request

import muse_jobs
import muse_voice
import muse_world

DATA_DIR = os.environ.get("MUSE_DATA_DIR") or os.path.expanduser("~/.config/muse")
CHAT_FILE = os.path.join(DATA_DIR, "chat.json")
API_URL = os.environ.get("MUSE_ANTHROPIC_URL", "https://api.anthropic.com/v1/messages")

KEEP = 300          # entries kept
JOIN_S = 20         # sentences of one spoken reply arrive one by one: join them into one entry
JOB_LEAD_S = 45     # a reply this soon after a job was started belongs above the job
TEXT_MAX = 2000
CONTEXT = 24        # recent entries the typed chat is given
TOOL_ROUNDS = 5

PROMPT = (
    "You are Muse, a tiny, round, cheerful creature who lives on a little screen on the user's desk. "
    "Right now the user is typing to you in your web app, so you answer in writing: warm, playful and "
    "short, one to three sentences of plain text, with no markdown, lists or emojis. The same "
    "conversation continues when they talk to you out loud; messages marked (said aloud) were spoken. "
    "You still have your body on the desk, and tools to move it. When the user asks you to do "
    "something physical (wave, dance, spin, come closer, go to sleep), do it by calling muse_do, and "
    "never just say you did it. Use muse_show to put a few words on your screen as a sign, and "
    "muse_set when asked to change your brightness or volume. Call muse_status for your battery, "
    "WiFi, how you are sitting and how noisy the room is, and time_now for the time or date; never "
    "guess those. Use timer when asked to time or remind, and remember when the user tells you "
    "something about themselves worth keeping. You cannot look things up yourself. When the user "
    "wants research, a comparison, options to choose from, or anything that needs current information "
    "from the web, call work_on with a complete, self-contained task and tell them you are on it; it "
    "takes a few minutes, the write-up then appears in this chat, and you announce it out loud on the "
    "desk. Call work_status when they ask how that is going or what you found. If the user asks what "
    "Claude Code is doing, call claude_status."
)


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


class Conversation:
    """What you and Muse have said to each other, newest last."""

    def __init__(self):
        try:
            with open(CHAT_FILE) as f:
                data = json.load(f)
            self.items = [e for e in data if isinstance(e, dict) and e.get("who") in ("you", "muse")][-KEEP:]
        except (OSError, ValueError):
            self.items = []

    def _save(self):
        try:
            os.makedirs(DATA_DIR, exist_ok=True)
            with open(CHAT_FILE + ".tmp", "w") as f:
                json.dump(self.items[-KEEP:], f)
            os.replace(CHAT_FILE + ".tmp", CHAT_FILE)
        except OSError as e:
            log("could not save the conversation:", e)

    def say(self, who, via, text):
        """Add something said or typed."""
        text = " ".join(str(text or "").split())[:TEXT_MAX]
        if not text:
            return
        now, at = time.time(), len(self.items)
        if who == "muse":
            # "On it!" comes after the job it is about was started: keep it above the job,
            # which is where the write-up will appear.
            while at and self.items[at - 1].get("job") and now - self.items[at - 1]["t"] < JOB_LEAD_S:
                at -= 1
        last = self.items[at - 1] if at else None
        if (via == "voice" and last and last.get("who") == who and last.get("via") == via and last.get("text")
                and now - last.get("end", last["t"]) < JOIN_S and len(last["text"]) + len(text) < TEXT_MAX):
            last["text"] += " " + text      # the next sentence of the same spoken turn
            last["end"] = now
        else:
            t = now
            if at < len(self.items):        # between what came before and the job
                t = (self.items[at]["t"] + (last["t"] if last else self.items[at]["t"] - 1)) / 2
            self.items.insert(at, {"t": t, "who": who, "via": via, "text": text})
        self.items = self.items[-KEEP:]
        self._save()

    def unsay(self, who, text):
        """Take back the last thing said, if it is this (a typed message that got no answer)."""
        if self.items and self.items[-1].get("who") == who and self.items[-1].get("text") == text:
            self.items.pop()
            self._save()

    def job(self, job_id):
        """Add that Muse started a background job."""
        self.items.append({"t": time.time(), "who": "muse", "job": job_id})
        self.items = self.items[-KEEP:]
        self._save()

    def recent(self, n=150):
        return [{k: v for k, v in e.items() if k != "end"} for e in self.items[-n:]]

    def history(self, n, mark_spoken=False):
        """The last `n` things said, as (role, text) for a model."""
        out = []
        for e in self.items[-n:]:
            if not e.get("text"):
                continue
            spoken = " (said aloud)" if mark_spoken and e.get("via") == "voice" and e["who"] == "you" else ""
            out.append(("user" if e["who"] == "you" else "assistant", e["text"][:600] + spoken))
        return out


class HubBody:
    """How a typed conversation reaches Muse: straight away, on every Muse connected."""

    def __init__(self, hub):
        self.hub = hub

    def do(self, moves):
        self.hub.broadcast({"type": "voice", "do": moves})

    def show(self, text, secs):
        self.hub.broadcast({"type": "voice", "show": text, "secs": secs})

    def set(self, chosen):
        self.hub.broadcast({"type": "voice", "set": chosen})

    def status_report(self):
        links = [d for d in self.hub.devices.values() if d.status]
        if not links:
            return {"error": "Muse is not connected right now"}
        return max(links, key=lambda d: d.status_at).status_report()


def tools():
    """The voice's tools, in the form the Anthropic API takes."""
    return [{"name": t["name"], "description": t["description"], "input_schema": t["parameters"]}
            for t in muse_voice.TOOLS]


def messages_from(history):
    """(role, text) pairs as API messages: alternating, starting with the user."""
    out = []
    for role, text in history:
        if out and out[-1]["role"] == role:
            out[-1]["content"] += "\n" + text
        elif out or role == "user":
            out.append({"role": role, "content": text})
    return out


def ask_api(key, model, system, messages):
    body = json.dumps({"model": model, "max_tokens": 700, "system": system, "messages": messages,
                       "tools": tools()}).encode()
    req = urllib.request.Request(API_URL, data=body, headers={
        "x-api-key": key, "anthropic-version": "2023-06-01", "content-type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            why = json.loads(e.read()).get("error", {}).get("message", "")
        except ValueError:
            why = ""
        raise RuntimeError("the model answered %d%s" % (e.code, ": " + why[:200] if why else ""))
    except (OSError, ValueError) as e:
        raise RuntimeError("could not reach the model (%s)" % (getattr(e, "reason", None) or type(e).__name__))


def fake(hub, text):
    """A stand-in for the model, for tests."""
    low = text.lower()
    if "dance" in low:
        muse_voice.run_tool("muse_do", {"moves": ["dance"]}, hub, HubBody(hub), "web")
    if "research" in low:
        muse_voice.run_tool("work_on", {"task": text, "title": text[:40]}, hub, HubBody(hub), "web")
    if "fail" in low:
        raise RuntimeError("asked to fail")
    return "Stand-in reply to: " + text[:80]


async def reply(hub, text):
    """Answer a typed message. Returns {"ok": True, "text": ...} or {"ok": False, "error": ...};
    both the message and the answer are added to the conversation."""
    text = " ".join(str(text or "").split())[:TEXT_MAX]
    if not text:
        return {"ok": False, "error": "type something first"}
    key = muse_jobs.anthropic_key()
    if not key and not os.environ.get("MUSE_CHAT_FAKE"):
        return {"ok": False, "error": "typing to Muse is not set up yet: ANTHROPIC_API_KEY is missing from .env"}
    hub.talk.say("you", "web", text)
    try:
        if os.environ.get("MUSE_CHAT_FAKE"):
            answer = fake(hub, text)
        else:
            answer = await _converse(hub, key)
    except RuntimeError as e:
        log("chat:", e)
        hub.talk.unsay("you", text)     # the page puts it back in the box to send again
        return {"ok": False, "error": str(e)}
    answer = answer.strip() or "Done!"
    hub.talk.say("muse", "web", answer)
    log("chat: you: %s | muse: %s" % (text[:80], answer[:120]))
    return {"ok": True, "text": answer}


async def _converse(hub, key):
    opts = muse_voice.options()
    t = muse_world.time_report(opts["timezone"])
    system = PROMPT + " It is %s, %s, around %s." % (t["weekday"], t["date"], t["time"])
    notes = muse_world.notes()
    if notes:
        system += " What you remember about the user: " + " ".join("- %s" % n for n in notes)
    messages = messages_from(hub.talk.history(CONTEXT, mark_spoken=True))
    loop, body, said = asyncio.get_event_loop(), HubBody(hub), []
    for _ in range(TOOL_ROUNDS):
        answer = await loop.run_in_executor(None, ask_api, key, opts["chat_model"], system, messages)
        content = answer.get("content") or []
        said += [b.get("text", "") for b in content if b.get("type") == "text"]
        calls = [b for b in content if b.get("type") == "tool_use"]
        if answer.get("stop_reason") != "tool_use" or not calls:
            break
        messages.append({"role": "assistant", "content": content})
        messages.append({"role": "user", "content": [
            {"type": "tool_result", "tool_use_id": c["id"],
             "content": muse_voice.run_tool(c.get("name"), c.get("input") or {}, hub, body, "web")} for c in calls]})
    return " ".join(s.strip() for s in said if s.strip())
