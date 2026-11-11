# SPDX-License-Identifier: MIT
"""What the AI knows about the world beyond the conversation: the time, timers,
what it has been asked to remember, and how Muse itself is doing.

These back the tools in muse_voice.py:

    time_now     the date and time where you are
    timer        set, list or cancel timers; when one is up Muse chirps, waves
                 and shows its label
    remember     keep a short note about you between conversations
    forget       drop notes again
    muse_status  what Muse's own sensors report (battery, WiFi, how it is
                 sitting, how noisy the room is ...)

Muse also speaks first; this module has the words for that: a timer that is
up, a low battery, and the note that asks the AI for a good-morning.

Notes are kept in memory.json in ~/.config/muse (or MUSE_DATA_DIR on a host).
The time zone is this computer's unless MUSE_TZ or voice.json's "timezone"
names another (an IANA name such as Australia/Sydney).
"""

import asyncio
import datetime
import json
import os
import time

DATA_DIR = os.environ.get("MUSE_DATA_DIR") or os.path.expanduser("~/.config/muse")
MEMORY_FILE = os.path.join(DATA_DIR, "memory.json")
STATE_FILE = os.path.join(DATA_DIR, "state.json")
MAX_NOTES = 40
NOTE_MAX = 160
MAX_TIMERS = 10


# ---------------------------------------------------------------- time
def now(zone=""):
    """The current time in `zone` (or MUSE_TZ, or this computer's zone)."""
    zone = os.environ.get("MUSE_TZ") or zone
    if zone:
        try:
            from zoneinfo import ZoneInfo  # Python 3.9+
            return datetime.datetime.now(ZoneInfo(zone))
        except Exception:
            pass
    return datetime.datetime.now().astimezone()


def time_report(zone=""):
    t = now(zone)
    return {
        "time": t.strftime("%I:%M %p").lstrip("0"),
        "weekday": t.strftime("%A"),
        "date": "%d %s" % (t.day, t.strftime("%B %Y")),
        "timezone": str(t.tzinfo) if t.tzinfo else "local",
    }


# ---------------------------------------------------------------- speaking first
QUIET_FROM_HOUR, QUIET_UNTIL_HOUR = 22, 8   # no battery warnings at night
MORNING_HOURS = range(5, 11)                # when picking Muse up earns a good morning
BATTERY_LOW, BATTERY_EMPTY, BATTERY_FINE = 20, 10, 30


def daytime(zone=""):
    """Not the hours when Muse keeps quiet unless asked."""
    return QUIET_UNTIL_HOUR <= now(zone).hour < QUIET_FROM_HOUR


GREETING_NOTE = ("(A note from your own senses, not something the user said: they have just picked you up "
                 "for the first time this morning. Say good morning in one short, warm sentence, and wave.)")


def timer_line(label):
    """What Muse says when a timer is up."""
    label = " ".join(str(label or "").split())
    if not label or label.lower().rstrip("!") == "time's up":
        return "Time's up!"
    return "Time's up! %s%s." % (label[0].upper(), label[1:])


def battery_line(percent, told):
    """(the level this warning is for, what Muse says, its sign), or None when
    there is nothing new to say. `told` is the level Muse last warned at (100:
    not yet); it is back to 100 once Muse is charging or well above low."""
    if percent <= BATTERY_EMPTY and told > BATTERY_EMPTY:
        return (BATTERY_EMPTY, "My battery is nearly empty, only %d percent. Please plug me in soon!" % percent,
                "Battery %d%%" % percent)
    if percent <= BATTERY_LOW and told > BATTERY_LOW:
        return (BATTERY_LOW, "I'm getting sleepy. My battery is at %d percent, could you plug me in?" % percent,
                "Battery %d%%" % percent)
    return None


def greeting_due(zone=""):
    """True once a day, in the morning. Remembered across restarts."""
    t = now(zone)
    today = t.date().isoformat()
    if t.hour not in MORNING_HOURS or _state().get("greeted") == today:
        return False
    _keep("greeted", today)
    return True


def _state():
    try:
        with open(STATE_FILE) as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _keep(key, value):
    data = dict(_state(), **{key: value})
    try:
        os.makedirs(DATA_DIR, exist_ok=True)
        with open(STATE_FILE + ".tmp", "w") as f:
            json.dump(data, f)
        os.replace(STATE_FILE + ".tmp", STATE_FILE)
    except OSError:
        pass


# ---------------------------------------------------------------- memory
def notes():
    try:
        with open(MEMORY_FILE) as f:
            data = json.load(f)
        return [str(n) for n in data if isinstance(n, str)][-MAX_NOTES:]
    except (OSError, ValueError, TypeError):
        return []


def _save(items):
    os.makedirs(DATA_DIR, exist_ok=True)
    tmp = MEMORY_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(items[-MAX_NOTES:], f, indent=2)
    os.replace(tmp, MEMORY_FILE)


def remember(note):
    note = " ".join(str(note).split())[:NOTE_MAX]
    if not note:
        return {"ok": False, "error": "nothing to remember"}
    items = [n for n in notes() if n.lower() != note.lower()] + [note]
    try:
        _save(items)
    except OSError as e:
        return {"ok": False, "error": "could not save: %s" % e}
    return {"ok": True, "remembered": note}


def forget(about):
    about = " ".join(str(about).split()).lower()
    items = notes()
    keep = [] if about in ("", "everything", "all") else [n for n in items if about not in n.lower()]
    try:
        _save(keep)
    except OSError as e:
        return {"ok": False, "error": "could not save: %s" % e}
    return {"ok": True, "forgotten": len(items) - len(keep), "still_remembered": len(keep)}


# ---------------------------------------------------------------- timers
class Timers:
    """Timers for everyone connected to this server. `alert(text)` is called
    when one is up."""

    def __init__(self, alert):
        self.alert = alert
        self.items = {}  # id -> {"label", "due" (time.time), "task"}
        self.next_id = 1

    def _minutes_left(self, item):
        return max(0.0, round((item["due"] - time.time()) / 60.0, 1))

    def set(self, minutes, label):
        try:
            minutes = float(minutes)
        except (TypeError, ValueError):
            return {"ok": False, "error": "say how many minutes"}
        if not 0 < minutes <= 24 * 60:
            return {"ok": False, "error": "a timer can be up to 24 hours"}
        if len(self.items) >= MAX_TIMERS:
            return {"ok": False, "error": "too many timers; cancel one first"}
        label = " ".join(str(label or "").split())[:18]
        tid, self.next_id = self.next_id, self.next_id + 1
        item = {"label": label, "due": time.time() + minutes * 60}
        item["task"] = asyncio.ensure_future(self._run(tid, minutes * 60))
        self.items[tid] = item
        return {"ok": True, "minutes": minutes, "label": label}

    async def _run(self, tid, seconds):
        await asyncio.sleep(seconds)
        item = self.items.pop(tid, None)
        if item:
            self.alert(item["label"] or "Time's up!")

    def list(self):
        return {"timers": [{"label": i["label"] or "timer", "minutes_left": self._minutes_left(i)}
                           for i in sorted(self.items.values(), key=lambda i: i["due"])]}

    def cancel(self, label):
        label = " ".join(str(label or "").split()).lower()
        gone = [tid for tid, i in self.items.items() if not label or label in i["label"].lower()]
        for tid in gone:
            self.items.pop(tid)["task"].cancel()
        return {"ok": True, "cancelled": len(gone), "left": len(self.items)}


# ---------------------------------------------------------------- Muse itself
def status_report(raw, age_s):
    """Muse's own report (its "status" message) in words the AI can use."""
    if not raw:
        return {"error": "Muse has not reported its status yet"}
    out = {}
    if raw.get("battery") is not None:
        out["battery_percent"] = raw["battery"]
        out["power"] = ("charging" if raw.get("charging") else
                        "plugged in" if raw.get("usb") else "running on battery")
    elif raw.get("usb") is not None:
        out["power"] = "plugged in, no battery found" if raw.get("usb") else "unknown"
    else:
        out["battery"] = "this board has no battery gauge that Muse can read"
    if raw.get("wifi"):
        rssi = raw.get("rssi", 0)
        out["wifi"] = "%s, %s signal" % (raw["wifi"], "strong" if rssi > -60 else "ok" if rssi > -72 else "weak")
    if raw.get("pose"):
        out["position"] = raw["pose"]
    if raw.get("noise") is not None:
        n = raw["noise"]
        out["room"] = "quiet" if n < 40 else "some noise" if n < 55 else "loud"
    for key in ("brightness", "volume"):
        if raw.get(key) is not None:
            out[key + "_percent"] = raw[key]
    if raw.get("uptime") is not None:
        out["awake_for_minutes"] = round(raw["uptime"] / 60)
    if raw.get("ram") is not None:
        out["free_memory_kb"] = {"internal": raw["ram"], "external": raw.get("psram")}
    out["reported_seconds_ago"] = int(age_s)
    return out
