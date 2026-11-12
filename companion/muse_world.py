# SPDX-License-Identifier: MIT
"""What the AI knows about the world beyond the conversation: the time, timers,
what it has been asked to remember, and how Muse itself is doing.

These back the tools in muse_voice.py:

    time_now     the date and time where you are
    timer        set, list or cancel timers; when one is up Muse says so, waves
                 and shows its label
    reminder     set, list or cancel reminders for a clock time, once or
                 repeating; they are kept in reminders.json, so they survive a
                 restart, and Muse says them when the time comes
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
import secrets
import time

DATA_DIR = os.environ.get("MUSE_DATA_DIR") or os.path.expanduser("~/.config/muse")
MEMORY_FILE = os.path.join(DATA_DIR, "memory.json")
STATE_FILE = os.path.join(DATA_DIR, "state.json")
REMINDERS_FILE = os.path.join(DATA_DIR, "reminders.json")
MAX_REMINDERS = 40
REMINDER_MAX = 120
LATE_S = 12 * 3600      # a reminder that was due while the server was off is still said, this long after
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


def briefing_note(today, missed, finished):
    """The note that asks the AI for the morning's greeting. With anything to
    report (`today`: reminders due today as "5:00 PM: call mum"; `missed`:
    reminders that came up while Muse was away; `finished`: titles of research
    that is ready) it becomes a short briefing."""
    if not (today or missed or finished):
        return GREETING_NOTE
    parts = []
    if today:
        parts.append("Reminders for today: " + "; ".join(today) + ".")
    if missed:
        parts.append("Reminders that came up while you were away: " + "; ".join(missed) + ".")
    if finished:
        parts.append("Research you finished since you last spoke, ready to read in the web app: " +
                     "; ".join(finished) + ".")
    return ("(A note from your own senses, not something the user said: they have just picked you up for the "
            "first time this morning. Say good morning and give them this briefing in two or three short "
            "spoken sentences, and wave. " + " ".join(parts) + ")")


def reminder_line(text, late=False):
    """What Muse says when a reminder is due."""
    text = " ".join(str(text).split())
    text = text + ("" if text[-1:] in ".!?" else ".")
    if late:
        return "Hey, a reminder from earlier: " + text
    return "Hey, reminder time! " + text[:1].upper() + text[1:]


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


def missed_reminders(add=None, clear=False):
    """Reminders that came up while no Muse was connected, kept until Muse has said them."""
    items = [str(x) for x in _state().get("missed", []) if isinstance(x, str)]
    if add or clear:
        _keep("missed", [] if clear else (items + [add])[-10:])
    return items


def finished_since_briefing(jobs, mark=False):
    """Titles of research that finished since the last morning briefing (or in the last day)."""
    since = max(float(_state().get("briefed") or 0), time.time() - 24 * 3600)
    titles = [j.get("title", "") for j in jobs if j.get("status") == "done"
              and j.get("created", 0) + (j.get("took_s") or 0) > since][:3]
    if mark:
        _keep("briefed", time.time())
    return titles


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


def forget_note(note):
    """Drop exactly this note (the web app's Forget button)."""
    note = " ".join(str(note).split()).lower()
    items = notes()
    keep = [n for n in items if n.lower() != note]
    try:
        _save(keep)
    except OSError as e:
        return {"ok": False, "error": "could not save: %s" % e}
    return {"ok": True, "forgotten": len(items) - len(keep)}


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


# ---------------------------------------------------------------- reminders
REPEATS = {"daily": "every day", "weekdays": "on weekdays", "weekly": "every week"}


def clock(t):
    return t.strftime("%I:%M %p").lstrip("0")


class Reminders:
    """Reminders for a clock time, once or repeating. `fire(reminder, late)` is
    called when one is due; `late` when it was due while the server was off.
    Each is {"id", "text", "at" (when it is next due, time.time), "repeat"}."""

    def __init__(self, fire, zone=lambda: ""):
        self.fire = fire
        # The last few that came due, for the AI: [text, time.time]. Kept across restarts.
        self.said = [x for x in _state().get("said", []) if isinstance(x, list) and len(x) == 2][-5:]
        self.zone = zone   # the user's time zone, asked for each time (voice.json can change)
        try:
            with open(REMINDERS_FILE) as f:
                data = json.load(f)
            self.items = [r for r in data if isinstance(r, dict) and r.get("text") and r.get("at")]
        except (OSError, ValueError):
            self.items = []

    def _save(self):
        try:
            os.makedirs(DATA_DIR, exist_ok=True)
            with open(REMINDERS_FILE + ".tmp", "w") as f:
                json.dump(self.items, f, indent=1)
            os.replace(REMINDERS_FILE + ".tmp", REMINDERS_FILE)
        except OSError:
            pass

    def _local(self, at):
        return datetime.datetime.fromtimestamp(at, now(self.zone()).tzinfo)

    def when(self, r):
        """When a reminder is due, in words: "today at 5:00 PM", "every day at 9:00 AM"."""
        t, today = self._local(r["at"]), now(self.zone()).date()
        if r.get("repeat") in REPEATS:
            day = " on %ss" % t.strftime("%A") if r["repeat"] == "weekly" else ""
            return "%s%s at %s" % (REPEATS[r["repeat"]], day, clock(t))
        days = (t.date() - today).days
        day = "today" if days == 0 else "tomorrow" if days == 1 else "on %s %d %s" % (
            t.strftime("%A"), t.day, t.strftime("%B"))
        return "%s at %s" % (day, clock(t))

    def _after(self, r):
        """The next time a repeating reminder is due after r["at"]."""
        t = self._local(r["at"])
        while True:
            t = datetime.datetime.combine(t.date() + datetime.timedelta(days=7 if r["repeat"] == "weekly" else 1),
                                          t.timetz().replace(fold=0))
            if (r["repeat"] != "weekdays" or t.weekday() < 5) and t.timestamp() > time.time():
                return t.timestamp()

    def set(self, text, at_time="", date="", in_minutes=None, repeat=""):
        text = " ".join(str(text or "").split())[:REMINDER_MAX]
        if not text:
            return {"ok": False, "error": "say what to remind about"}
        if len(self.items) >= MAX_REMINDERS:
            return {"ok": False, "error": "too many reminders; cancel some first"}
        repeat = repeat if repeat in REPEATS else ""
        current = now(self.zone())
        try:
            if in_minutes not in (None, ""):
                minutes = float(in_minutes)
                if not 0 < minutes <= 366 * 24 * 60:
                    raise ValueError
                due = current + datetime.timedelta(minutes=minutes)
            else:
                hour, minute = (int(x) for x in str(at_time).strip().split(":")[:2])
                day = datetime.date.fromisoformat(str(date).strip()) if date else current.date()
                due = datetime.datetime.combine(day, datetime.time(hour, minute), current.tzinfo)
                if not date and due <= current:
                    due += datetime.timedelta(days=1)       # "at 7" said after 7: tomorrow
        except (TypeError, ValueError):
            return {"ok": False, "error": "give the time as HH:MM on a 24-hour clock (and the date as "
                                          "YYYY-MM-DD), or in_minutes"}
        r = {"id": secrets.token_hex(4), "text": text, "at": due.timestamp(), "repeat": repeat}
        if repeat == "weekdays" and due.weekday() >= 5 or due <= current and repeat:
            r["at"] = self._after(r)
        elif due <= current:
            return {"ok": False, "error": "that time has already passed"}
        self.items.append(r)
        self._save()
        return {"ok": True, "reminder": text, "when": self.when(r)}

    def list(self):
        return {"reminders": [{"id": r["id"], "text": r["text"], "when": self.when(r)}
                              for r in sorted(self.items, key=lambda r: r["at"])]}

    def _said_when(self, at):
        day = self._local(at)
        today = now(self.zone()).date()
        return ("today" if day.date() == today else day.strftime("on %A")) + " at " + clock(day)

    def for_ai(self):
        """What the AI is told when it asks which reminders there are."""
        return {"reminders": [{"text": r["text"], "when": self.when(r)} for r in sorted(self.items, key=lambda r: r["at"])],
                "already_said": [{"text": text, "said": self._said_when(at)} for text, at in self.said],
                "note": "A reminder that does not repeat leaves this list once Muse has said it out loud; "
                        "one that is missing was delivered, not lost."}

    def cancel(self, which):
        """Cancel by id, by words in the text, or "all"."""
        which = " ".join(str(which or "").split()).lower()
        if not which:
            return {"ok": False, "error": "say which reminder, or 'all'"}
        gone = [r for r in self.items if which == r["id"]] or \
               [r for r in self.items if which in ("all", "everything") or which in r["text"].lower()]
        self.items = [r for r in self.items if r not in gone]
        self._save()
        return {"ok": True, "cancelled": [r["text"] for r in gone], "left": len(self.items)}

    def today(self):
        """Reminders still to come today, as "5:00 PM: call mum"."""
        current = now(self.zone())
        return ["%s: %s" % (clock(self._local(r["at"])), r["text"]) for r in sorted(self.items, key=lambda r: r["at"])
                if r["at"] > time.time() and self._local(r["at"]).date() == current.date()]

    def check(self):
        """Fire what is due. Called every few seconds."""
        due = [r for r in self.items if r["at"] <= time.time()]
        for r in due:
            late = time.time() - r["at"]
            if r.get("repeat") in REPEATS:
                r["at"] = self._after(r)
            else:
                self.items.remove(r)
            if late < LATE_S:
                self.said = (self.said + [[r["text"], time.time()]])[-5:]
                _keep("said", self.said)
                self.fire(r, late > 120)
        if due:
            self._save()

    async def run(self):
        while True:
            try:
                self.check()
            except Exception as e:      # one bad reminder must not stop the rest
                print(time.strftime("%H:%M:%S"), "reminders:", type(e).__name__, e, flush=True)
            await asyncio.sleep(10)


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
