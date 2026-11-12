# SPDX-License-Identifier: MIT
"""Firmware updates over WiFi: the image Muse should be running.

The online server keeps one firmware image, firmware.bin in ~/.config/muse (or
MUSE_DATA_DIR), with what it is in firmware.json. "muse_companion.py update"
puts a build there (POST /firmware). A Muse that links says which image it is
running; if it is another one, the server offers this one, and Muse downloads
it from /firmware.bin when it has been left alone for a moment
(firmware/main/update.c). Both addresses always need the token.

An image is known by the SHA-256 that ESP-IDF puts at its end (its "id"), which
is also what Muse reports about the image it is running, so nothing has to keep
count of versions.
"""

import hashlib
import json
import os
import time

DATA_DIR = os.environ.get("MUSE_DATA_DIR") or os.path.expanduser("~/.config/muse")
BIN_FILE = os.path.join(DATA_DIR, "firmware.bin")
INFO_FILE = os.path.join(DATA_DIR, "firmware.json")

PROJECT = "muse_charm"          # project() in firmware/CMakeLists.txt
MAX_BYTES = 0x7F0000            # an app slot in firmware/partitions.csv
CHIP_ESP32S3 = 9
APP_DESC_AT = 32                # after the image header and the first segment's header
APP_DESC_MAGIC = bytes.fromhex("3254cdab")


def _text(raw):
    return raw.split(b"\0", 1)[0].decode("ascii", "replace").strip()


def describe(data):
    """What a firmware image is: {"id", "size", "version", "built"}. ValueError says why it is not one."""
    if len(data) < 4096:
        raise ValueError("that is not a firmware image")
    if len(data) > MAX_BYTES:
        raise ValueError("the image is larger than Muse's app slot (%.1f MB)" % (MAX_BYTES / 1e6))
    if data[0] != 0xE9 or data[APP_DESC_AT:APP_DESC_AT + 4] != APP_DESC_MAGIC:
        raise ValueError("that is not an ESP-IDF app image (expected firmware/build/muse_charm.bin)")
    if int.from_bytes(data[12:14], "little") != CHIP_ESP32S3:
        raise ValueError("the image was not built for the ESP32-S3")
    if data[23] != 1 or hashlib.sha256(data[:-32]).digest() != data[-32:]:
        raise ValueError("the image is damaged (its checksum does not match)")
    d = data[APP_DESC_AT:APP_DESC_AT + 256]
    version, project, clock, date = _text(d[16:48]), _text(d[48:80]), _text(d[80:96]), _text(d[96:112])
    if project != PROJECT:
        raise ValueError("that is firmware for %r, not for Muse" % project)
    return {"id": data[-32:].hex(), "size": len(data), "version": version, "built": ("%s %s" % (date, clock)).strip()}


def put(data):
    """Keep this image as the one Muse should run. Returns what it is; ValueError if it is not Muse firmware."""
    info = dict(describe(data), uploaded=time.time())
    os.makedirs(DATA_DIR, exist_ok=True)
    for path, content in ((BIN_FILE, data), (INFO_FILE, json.dumps(info).encode())):
        with open(path + ".tmp", "wb") as f:
            f.write(content)
        os.replace(path + ".tmp", path)
    return info


def info():
    """The image the server has, or None."""
    try:
        with open(INFO_FILE) as f:
            found = json.load(f)
        if isinstance(found, dict) and found.get("id") and os.path.getsize(BIN_FILE) == found.get("size"):
            return found
    except (OSError, ValueError):
        pass
    return None


def read():
    """The image itself, or None."""
    try:
        with open(BIN_FILE, "rb") as f:
            return f.read()
    except OSError:
        return None
