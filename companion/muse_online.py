# SPDX-License-Identifier: MIT
"""Where the online companion server is, for the tools on this computer.

The password ("token") that Muse, the Claude Code hooks and the web app's
sign-in present to it is MUSE_TOKEN in the repository's .env file, next to
your keys. ~/.config/muse/server.json holds the server's address (and a copy of
the token, which is used when .env has none):

    {"url": "wss://your-app.example.com/device", "token": "..."}

deploy_modal.py writes both. Neither goes into the repository.
"""

import json
import os
import secrets

import muse_env

TOKEN_MIN = 16

CONFIG_DIR = os.path.expanduser("~/.config/muse")
SERVER_FILE = os.path.join(CONFIG_DIR, "server.json")


def load():
    """The saved settings ({} if there are none). The token is the one in .env
    when there is one."""
    try:
        with open(SERVER_FILE) as f:
            data = json.load(f)
        data = data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        data = {}
    if muse_env.get("MUSE_TOKEN"):
        data["token"] = muse_env.get("MUSE_TOKEN")
    return data


def save(data):
    os.makedirs(CONFIG_DIR, exist_ok=True)
    tmp = SERVER_FILE + ".tmp"
    with open(os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.replace(tmp, SERVER_FILE)


def token():
    """The token: MUSE_TOKEN from .env, else the saved one, else a new one. It is
    written to .env (so that is where you find it) and to server.json."""
    data = load()
    if not data.get("token"):
        data["token"] = secrets.token_urlsafe(24)
    if len(data["token"]) < TOKEN_MIN or any(c.isspace() for c in data["token"]):
        raise SystemExit("MUSE_TOKEN in %s must be at least %d characters with no spaces: it is all that keeps "
                         "strangers from using your server and your keys." % (muse_env.ENV_FILE, TOKEN_MIN))
    save(data)
    if muse_env.get("MUSE_TOKEN") != data["token"]:
        muse_env.put("MUSE_TOKEN", data["token"])
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
