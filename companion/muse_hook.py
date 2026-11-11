#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Claude Code hook for an online Muse companion server.

Claude Code runs this for each event (install_hooks.py --online sets that up)
and passes the event on standard input. The event can hold your prompt, file
contents and commands, so this script keeps only what Muse shows:

    which event, a session id, the project folder's name, the tool's name, and
    for a tool call the file's name, the website's host or the one-line
    description Claude gives a shell command

and sends that to the server in ~/.config/muse/server.json. The sending
happens in the background, so Claude Code never waits for the network, and
this always succeeds whether or not the server can be reached.
"""

import json
import os
import subprocess
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import muse_online  # noqa: E402

KEEP = ("hook_event_name", "session_id", "source", "tool_name", "tool_use_id", "notification_type", "agent_id",
        "is_interrupt")


def summary(ev):
    """The few fields the server uses, with nothing private in them."""
    out = {k: ev[k] for k in KEEP if isinstance(ev.get(k), (str, bool, int))}
    if ev.get("cwd"):
        out["cwd"] = os.path.basename(str(ev["cwd"]).rstrip("/"))  # the folder's name, not its path
    ti = ev.get("tool_input")
    if isinstance(ti, dict):
        small = {}
        if isinstance(ti.get("description"), str):
            small["description"] = " ".join(ti["description"].split())[:80]
        for key in ("file_path", "notebook_path"):
            if isinstance(ti.get(key), str):
                small[key] = os.path.basename(ti[key].rstrip("/"))
        if isinstance(ti.get("url"), str):
            small["url"] = "/".join(ti["url"].split("/")[:3]) + "/"  # scheme and host only
        if small:
            out["tool_input"] = small
    return out


def send(payload):
    saved = muse_online.load()
    if not saved.get("url"):
        return
    req = urllib.request.Request(muse_online.http_url(saved["url"], "/hook"), data=payload,
                                 headers={"Content-Type": "application/json",
                                          "Authorization": "Bearer " + saved.get("token", "")})
    urllib.request.urlopen(req, timeout=8).read()


def main():
    try:
        if "--send" in sys.argv:  # the background half
            send(sys.stdin.buffer.read())
            return
        ev = json.loads(sys.stdin.read() or "{}")
        if not isinstance(ev, dict):
            return
        payload = json.dumps(summary(ev)).encode()
        if "--print" in sys.argv:  # show what would be sent, and send nothing
            print(payload.decode())
            return
        child = subprocess.Popen([sys.executable, os.path.abspath(__file__), "--send"], stdin=subprocess.PIPE,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        child.stdin.write(payload)
        child.stdin.close()
    except Exception:
        pass  # never get in Claude Code's way


if __name__ == "__main__":
    main()
