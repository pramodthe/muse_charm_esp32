# SPDX-License-Identifier: MIT
"""A small websocket implementation on asyncio streams (standard library only).

Enough for the companion: the server side of Muse's connection, and a client
for the Deepgram Voice Agent API.
"""

import asyncio
import base64
import hashlib
import os
import ssl
import struct
import urllib.parse

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_MESSAGE = 4 * 1024 * 1024

TEXT, BINARY, CLOSE, PING, PONG = 0x1, 0x2, 0x8, 0x9, 0xA


def _xor(data, mask):
    if not data:
        return data
    n = len(data)
    m = (mask * (n // 4 + 1))[:n]
    return (int.from_bytes(data, "big") ^ int.from_bytes(m, "big")).to_bytes(n, "big")


def frame(op, payload, masked=False):
    """One complete frame. Clients must mask what they send; servers must not."""
    n = len(payload)
    bit = 0x80 if masked else 0
    if n < 126:
        head = struct.pack("!BB", 0x80 | op, bit | n)
    elif n < 65536:
        head = struct.pack("!BBH", 0x80 | op, bit | 126, n)
    else:
        head = struct.pack("!BBQ", 0x80 | op, bit | 127, n)
    if not masked:
        return head + payload
    mask = os.urandom(4)
    return head + mask + _xor(payload, mask)


async def read_frame(reader):
    """(fin, opcode, payload) of the next frame."""
    b1, b2 = await reader.readexactly(2)
    n = b2 & 0x7F
    if n == 126:
        (n,) = struct.unpack("!H", await reader.readexactly(2))
    elif n == 127:
        (n,) = struct.unpack("!Q", await reader.readexactly(8))
    if n > MAX_MESSAGE:
        raise ConnectionError("websocket frame too large")
    mask = await reader.readexactly(4) if b2 & 0x80 else None
    data = await reader.readexactly(n)
    if mask:
        data = _xor(data, mask)
    return bool(b1 & 0x80), b1 & 0x0F, data


async def read_message(reader, writer, masked_replies=False, frame_timeout=None):
    """(opcode, payload) of the next text, binary or close message. Answers
    pings and joins fragmented messages. frame_timeout limits the wait for each
    frame (pings included), so a quiet but live peer is not mistaken for a
    dead one."""
    parts, op = [], None
    while True:
        if frame_timeout:
            fin, code, data = await asyncio.wait_for(read_frame(reader), frame_timeout)
        else:
            fin, code, data = await read_frame(reader)
        if code == PING:
            writer.write(frame(PONG, data, masked_replies))
            continue
        if code == PONG:
            continue
        if code == CLOSE:
            return CLOSE, data
        if code != 0:
            op, parts = code, []
        parts.append(data)
        if fin and op is not None:
            return op, b"".join(parts)


def accept_key(key):
    return base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()


async def connect(url, headers=None, timeout=10):
    """Open a client websocket. Returns (reader, writer)."""
    u = urllib.parse.urlsplit(url)
    secure = u.scheme == "wss"
    port = u.port or (443 if secure else 80)
    ctx = ssl.create_default_context() if secure else None
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(u.hostname, port, ssl=ctx, server_hostname=u.hostname if secure else None,
                                limit=1 << 16),
        timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    path = u.path or "/"
    if u.query:
        path += "?" + u.query
    lines = ["GET %s HTTP/1.1" % path, "Host: %s" % u.hostname, "Upgrade: websocket",
             "Connection: Upgrade", "Sec-WebSocket-Key: %s" % key, "Sec-WebSocket-Version: 13"]
    for k, v in (headers or {}).items():
        lines.append("%s: %s" % (k, v))
    writer.write(("\r\n".join(lines) + "\r\n\r\n").encode())
    head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout)
    status = head.split(b"\r\n", 1)[0].decode("latin-1")
    if " 101 " not in status + " ":
        body = b""
        try:
            body = await asyncio.wait_for(reader.read(400), 2)
        except (asyncio.TimeoutError, OSError):
            pass
        writer.close()
        raise ConnectionError("%s %s" % (status, body.decode("utf-8", "replace").strip()))
    return reader, writer
