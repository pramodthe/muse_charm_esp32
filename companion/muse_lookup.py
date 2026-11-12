# SPDX-License-Identifier: MIT
"""Quick answers from the web, in a few seconds, for Muse to say.

The AI calls its look_up tool with a question ("what's the weather in Sydney
today?"). look_up() here asks Claude, through the Anthropic API, to search the
web once or twice and answer in one to three short sentences that can be
spoken. Bigger questions, which need reading and a write-up, are background
jobs instead (muse_jobs.py).

It uses the same key as the background jobs (ANTHROPIC_API_KEY in .env) and
the basic web search tool, which is the fastest. Searches cost money on the
Anthropic account (see Anthropic's pricing); "lookup_searches" in voice.json
sets how many one question may use.

The answer comes back with the pages it was taken from, which the web app shows
under the conversation as links. Only the standard library is used.

MUSE_LOOKUP_FAKE=1 answers without the API (for testing); MUSE_LOOKUP_FAKE_S
sets how long that takes.
"""

import json
import os
import re
import time
import urllib.error
import urllib.parse
import urllib.request

import muse_jobs
import muse_world

API_URL = os.environ.get("MUSE_ANTHROPIC_URL", "https://api.anthropic.com/v1/messages")
SEARCH_TOOL = "web_search_20250305"   # basic search; the newer versions filter results with code, which is slower
TIMEOUT_S = 30
QUESTION_MAX = 400
ANSWER_MAX = 600
SOURCES_MAX = 3
MAX_TOKENS = 1500     # the model thinks a little first, and that counts too
EFFORT = "low"        # how hard it thinks: low answers a quick question in 2 to 6 seconds

INSTRUCTIONS = (
    "You answer one question for Muse, a small voice companion on the user's desk. Search the web "
    "for current information, using as few searches as you can, then reply with only the answer: "
    "one or two short, plain sentences (three at most) that will be spoken aloud. Put the answer "
    "first, with the numbers, names and times that matter, in the units and currency people use "
    "where the user is. No markdown, lists, links, source names in brackets or remarks about "
    "searching. If the sources disagree or you could not find it, say so in a sentence."
)


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


def _zone(zone):
    """The user's IANA time zone, if there is a valid one."""
    zone = os.environ.get("MUSE_TZ") or zone
    if zone and "/" in zone and not zone.startswith("Etc/"):
        try:
            from zoneinfo import ZoneInfo
            ZoneInfo(zone)
            return zone
        except Exception:
            pass
    return ""


def place_name(zone, place):
    """Where the user is, in words: the place from voice.json, else the city in the time zone's name."""
    zone = _zone(zone)
    return (place or "").strip() or (zone.rsplit("/", 1)[-1].replace("_", " ") if zone else "")


def where(zone, place):
    """The search's user_location: the place from voice.json, else the city in the time zone's name."""
    zone = _zone(zone)
    city = (place or "").split(",")[0].strip() or (zone.rsplit("/", 1)[-1].replace("_", " ") if zone else "")
    loc = {"type": "approximate"}
    if city:
        loc["city"] = city[:60]
    if zone:
        loc["timezone"] = zone
    return loc if len(loc) > 1 else None


def _clean(text):
    text = re.sub(r"[*_`#]+", "", text)            # stray markdown
    text = re.sub(r"\[(\d+|source)\]", "", text)    # citation marks
    text = " ".join(text.split())
    if len(text) > ANSWER_MAX:
        text = text[:ANSWER_MAX].rsplit(". ", 1)[0] + "."
    return text


def _site(url):
    host = urllib.parse.urlsplit(url).netloc.lower()
    return host[4:] if host.startswith("www.") else host


def read_answer(content):
    """(answer, sources) from the API's content blocks: the text after the last
    search, and the pages it cites ({"title", "url", "site"}). An answer that
    cites nothing gets the first pages its last search found."""
    searches = [i for i, b in enumerate(content) if b.get("type") == "web_search_tool_result"]
    texts, sources, seen = [], [], set()

    def add(url, title):
        url = str(url or "")
        if url.startswith(("http://", "https://")) and url not in seen and len(sources) < SOURCES_MAX:
            seen.add(url)
            sources.append({"title": " ".join(str(title or _site(url)).split())[:120], "url": url, "site": _site(url)})

    for block in content[(searches[-1] + 1 if searches else 0):]:
        if block.get("type") == "text":
            texts.append(block.get("text", ""))
            for c in block.get("citations") or []:
                add(c.get("url"), c.get("title"))
    if not sources and searches and isinstance(content[searches[-1]].get("content"), list):
        for found in content[searches[-1]]["content"][:2]:
            add(found.get("url"), found.get("title"))
    return _clean("".join(texts)), sources


def _ask(key, body):
    req = urllib.request.Request(API_URL, data=json.dumps(body).encode(), headers={
        "x-api-key": key, "anthropic-version": "2023-06-01", "content-type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            why = json.loads(e.read()).get("error", {}).get("message", "")
        except ValueError:
            why = ""
        raise RuntimeError("the model answered %d%s" % (e.code, ": " + why[:200] if why else ""))
    except (OSError, ValueError) as e:
        raise RuntimeError("could not reach the model (%s)" % (getattr(e, "reason", None) or type(e).__name__))


def fake(question):
    time.sleep(float(os.environ.get("MUSE_LOOKUP_FAKE_S") or 1))
    if "fail" in question.lower():
        return {"ok": False, "error": "asked to fail"}
    return {"ok": True, "answer": "Stand-in answer to: %s" % question[:80],
            "sources": [{"title": "Example", "url": "https://example.com/answer", "site": "example.com"}]}


def look_up(question, model, zone="", place="", notes=(), searches=3):
    """Answer `question` from the web. Returns {"ok": True, "answer", "sources"}
    or {"ok": False, "error"}. Blocks for a few seconds: call it off the event loop."""
    question = " ".join(str(question or "").split())[:QUESTION_MAX]
    if len(question) < 4:
        return {"ok": False, "error": "ask a full question"}
    if os.environ.get("MUSE_LOOKUP_FAKE"):
        return fake(question)
    key = muse_jobs.anthropic_key()
    if not key:
        return {"ok": False, "error": "looking things up is not set up yet: ANTHROPIC_API_KEY is missing from .env"}
    t = muse_world.time_report(zone)
    system = INSTRUCTIONS + " It is %s, %s, %s (%s)." % (t["weekday"], t["date"], t["time"], t["timezone"])
    if place:
        system += " The user is in %s." % place
    if notes:
        system += " What Muse knows about the user (use it only if it helps): " + " ".join("- %s" % n for n in notes)
    search = {"type": SEARCH_TOOL, "name": "web_search", "max_uses": max(1, min(5, int(searches or 3)))}
    loc = where(zone, place)
    if loc:
        search["user_location"] = loc
    asked = {"role": "user", "content": question}
    so_far = []      # the answer's content blocks, across pauses
    try:
        for _ in range(3):
            messages = [asked] + ([{"role": "assistant", "content": so_far}] if so_far else [])
            answer = _ask(key, {"model": model, "max_tokens": MAX_TOKENS, "system": system,
                                "messages": messages, "tools": [search], "output_config": {"effort": EFFORT}})
            so_far = so_far + (answer.get("content") or [])
            if answer.get("stop_reason") != "pause_turn":   # a long turn is paused: carry on where it stopped
                break
        text, sources = read_answer(so_far)
    except RuntimeError as e:
        return {"ok": False, "error": str(e)}
    if answer.get("stop_reason") == "max_tokens":
        log("look_up: the answer was cut off at %d tokens" % MAX_TOKENS)
        text = text.rsplit(". ", 1)[0] + "." if ". " in text else ""   # keep the whole sentences
    if not text:
        return {"ok": False, "error": "no answer came back"}
    return {"ok": True, "answer": text, "sources": sources}
