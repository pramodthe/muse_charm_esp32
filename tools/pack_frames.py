#!/usr/bin/env python3
"""Pack Muse's clips into the binary table the firmware embeds ("MUS2").

The firmware build runs this automatically (see firmware/main/CMakeLists.txt):

    python3 tools/pack_frames.py assets/frames assets/anchors.json build/muse_frames.bin

The frames are the Blender renders with the face hidden; the firmware draws the
face live at the per-frame anchors exported by blender/export_clips.py.

Layout (little endian):
  header  "MUS2" u16 width u16 height u16 nframes u16 nclips u16 belly_x u16 belly_y u16 eye_w u16 eye_h
  clips   nclips x { char name[8]; u16 first; u16 count; u8 fps; u8 flags; u16 reserved }
  anchors nframes x { i16 eye_l_x, eye_l_y, eye_r_x, eye_r_y, mouth_x, mouth_y (pixels x4);
                      i16 roll (degrees x10); u8 expression; u8 reserved }
  offsets u32[nframes + 1] into the JPEG data
  data    baseline JPEG frames
"""
import glob
import json
import os
import struct
import sys

if len(sys.argv) != 4:
    sys.exit("usage: pack_frames.py <frames_dir> <anchors.json> <out.bin>")
folder, anchors_path, out = sys.argv[1], sys.argv[2], sys.argv[3]
meta = json.load(open(anchors_path))
files = sorted(glob.glob(os.path.join(folder, "*.jpg")))
blobs = [open(f, "rb").read() for f in files]
assert len(blobs) == len(meta["frames"]), f"{len(blobs)} jpgs but {len(meta['frames'])} anchor rows"


def jpeg_size(b):
    i = 2
    while i < len(b):
        if b[i] != 0xFF:
            i += 1
            continue
        marker = b[i + 1]
        if marker in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", b[i + 5:i + 9])
            return w, h, marker
        i += 2 + struct.unpack(">H", b[i + 2:i + 4])[0]
    raise ValueError("no SOF marker")


sizes = {jpeg_size(b) for b in blobs}
assert len(sizes) == 1, f"frames differ: {sizes}"
w, h, sof = sizes.pop()
assert sof == 0xC0, "frames must be baseline JPEG"
assert (w, h) == (meta["width"], meta["height"])

clips = meta["clips"]
eye_w, eye_h = meta.get("rest_eye_size", [10, 13])
bx, by = meta["belly"]
parts = [b"MUS2" + struct.pack("<8H", w, h, len(blobs), len(clips), int(bx), int(by), eye_w, eye_h)]
for c in clips:
    name = c["name"].encode()[:8].ljust(8, b"\0")
    parts.append(name + struct.pack("<HHBBH", c["first"], c["count"], c["fps"], c["flags"], 0))
for r in meta["frames"]:
    q = [int(round(v * 4)) for v in r[:6]]
    parts.append(struct.pack("<7hBB", *q, int(round(r[6] * 10)), int(r[7]), 0))
offsets, pos = [], 0
for b in blobs:
    offsets.append(pos)
    pos += len(b)
offsets.append(pos)
parts.append(struct.pack(f"<{len(offsets)}I", *offsets))
head = b"".join(parts)
assert len(head) % 4 == 0
with open(out, "wb") as f:
    f.write(head)
    for b in blobs:
        f.write(b)
print(f"{len(blobs)} frames {w}x{h}, {len(clips)} clips -> {out} ({os.path.getsize(out)} bytes)")
for c in clips:
    print(f"  {c['name']:8s} first {c['first']:3d} count {c['count']:3d} fps {c['fps']} flags {c['flags']}")
