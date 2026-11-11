# SPDX-License-Identifier: MIT
"""Where the online companion server is, for the tools on this computer.

~/.config/muse/server.json holds its address and the password ("token") Muse
and the Claude Code hooks present to it:

    {"url": "wss://your-app.example.com/device", "token": "..."}

deploy_modal.py writes this file. It never goes into the repository.
"""

import json
import os
import secrets

CONFIG_DIR = os.path.expanduser("~/.config/muse")
SERVER_FILE = os.path.join(CONFIG_DIR, "server.json")


def load():
    """The saved settings ({} if there are none)."""
    try:
        with open(SERVER_FILE) as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save(data):
    os.makedirs(CONFIG_DIR, exist_ok=True)
    tmp = SERVER_FILE + ".tmp"
    with open(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.replace(tmp, SERVER_FILE)


def token():
    """The saved token, made (and saved) on first use."""
    data = load()
    if not data.get("token"):
        data["token"] = secrets.token_urlsafe(24)
        save(data)
    return data["token"]


def device_url(base):
    """wss://host/device from whatever form the address was given in."""
    base = base.strip().rstrip("/")
    if base.endswith("/device"):
        base = base[: -len("/device")]
    for http, ws in (("https://", "wss://"), ("http://", "ws://")):
        if base.startswith(http):
            base = ws + base[len(http):]
    if "://" not in base:
        base = "wss://" + base
    return base + "/device"


def http_url(url, path):
    """The server's https://host/<path> address, from its websocket address."""
    base = device_url(url)[: -len("/device")]
    for ws, http in (("wss://", "https://"), ("ws://", "http://")):
        if base.startswith(ws):
            base = http + base[len(ws):]
    return base + path
