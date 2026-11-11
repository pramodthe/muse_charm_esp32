# SPDX-License-Identifier: MIT
"""Your keys, from the .env file in the repository's folder.

    cp .env.example .env        then paste your keys into .env

.env is in .gitignore, so it never goes to GitHub. A variable that is set in
the environment wins over the file (that is how a host such as Modal hands the
keys over), and the older key files in ~/.config/muse still work when .env has
no value. The file is read again whenever it changes, so a running server
notices a new key.
"""

import os

ENV_FILE = os.environ.get("MUSE_ENV_FILE") or os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".env")

_cache = {"mtime": None, "values": {}}


def parse(text):
    """KEY=value lines; blank lines, comments and a leading "export " are ignored."""
    values = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, value = line.split("=", 1)
        name, value = name.replace("export ", "", 1).strip(), value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        elif " #" in value:
            value = value.split(" #", 1)[0].rstrip()
        if name:
            values[name] = value
    return values


def _file():
    try:
        mtime = os.path.getmtime(ENV_FILE)
        if mtime != _cache["mtime"]:
            with open(ENV_FILE) as f:
                _cache.update(mtime=mtime, values=parse(f.read()))
    except OSError:
        _cache.update(mtime=None, values={})
    return _cache["values"]


def get(name):
    """The value of `name`: from the environment, else from .env, else ""."""
    return (os.environ.get(name) or "").strip() or _file().get(name, "")
