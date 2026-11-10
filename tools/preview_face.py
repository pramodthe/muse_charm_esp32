#!/usr/bin/env python3
"""Preview the clips on your computer, with the face drawn the way the firmware
draws it, without flashing a board.

    python3 tools/preview_face.py                     # every clip -> build/preview/muse_all.gif
    python3 tools/preview_face.py --clips wave giggle # just these clips
    python3 tools/preview_face.py --sheet             # expression sheet PNG

Needs Pillow (pip install pillow). The drawing is an approximation of the LVGL
face in firmware/main/face.c, close enough to check anchors and expressions.
"""
import argparse
import json
import math
import os

from PIL import Image, ImageDraw

COLORS = dict(eye=(0x3A, 0x2F, 0x29), highlight=(0xFF, 0xF6, 0xEE), line=(0x4A, 0x38, 0x30),
              mouth=(0x5A, 0x2A, 0x28), tongue=(0xE0, 0x7A, 0x80))
NORMAL, HAPPY, SQUEEZE, DIZZY, TALK, SURPRISED = range(6)
SS = 3  # supersampling for smooth edges


def rotate(r, dx, dy):
    c, s = math.cos(r), math.sin(r)
    return dx * c - dy * s, dx * s + dy * c


def draw_face(img, row, expr=None, gaze=(0.0, 0.0), level=0.0, spin=0, eye_size=(10, 13)):
    elx, ely, erx, ery, mx, my, roll, frame_expr = row
    expr = frame_expr if expr is None else expr
    big = img.resize((img.width * SS, img.height * SS))
    d = ImageDraw.Draw(big)
    r = math.radians(roll)

    def ellipse(cx, cy, w, h, col):
        d.ellipse([(cx - w / 2) * SS, (cy - h / 2) * SS, (cx + w / 2) * SS, (cy + h / 2) * SS], fill=col)

    def arc(cx, cy, size, a0, a1, width, col, rot=0.0):
        d.arc([(cx - size / 2) * SS, (cy - size / 2) * SS, (cx + size / 2) * SS, (cy + size / 2) * SS],
              a0 + rot, a1 + rot, fill=col, width=int(width * SS))

    gx, gy = gaze[0] * 3.5, gaze[1] * 2.5
    for i, (ex, ey) in enumerate(((elx, ely), (erx, ery))):
        if expr == HAPPY:
            ox, oy = rotate(r, 0, 4)
            arc(ex + ox, ey + oy, 13, 205, 335, 3, COLORS["line"], roll)
        elif expr == SQUEEZE:
            outer, inner = (2, 8) if i == 0 else (8, 2)
            x0, y0 = ex - 5, ey - 6.5
            d.line([((x0 + outer) * SS, (y0 + 2) * SS), ((x0 + inner) * SS, (y0 + 6) * SS),
                    ((x0 + outer) * SS, (y0 + 11) * SS)], fill=COLORS["line"], width=3 * SS, joint="curve")
        elif expr == DIZZY:
            arc(ex, ey, 12, 0, 290, 2, COLORS["eye"], spin if i == 0 else 360 - spin)
        else:
            w, h = eye_size if expr != SURPRISED else (eye_size[0] + 2, eye_size[1] + 2)
            ellipse(ex + gx, ey + gy, w, h, COLORS["eye"])
            ellipse(ex + gx - w * 0.22, ey + gy - h * 0.25, 3, 3, COLORS["highlight"])
    if expr == NORMAL:
        ox, oy = rotate(r, 0, -6)
        arc(mx + ox, my + oy, 20, 35, 145, 3, COLORS["line"], roll)
    elif expr in (HAPPY, SQUEEZE):
        ox, oy = rotate(r, 0, -2.5)
        d.pieslice([(mx + ox - 9) * SS, (my + oy - 9) * SS, (mx + ox + 9) * SS, (my + oy + 9) * SS],
                   roll, 180 + roll, fill=COLORS["mouth"])
        ox, oy = rotate(r, 0, 3.5)
        ellipse(mx + ox, my + oy, 8, 4, COLORS["tongue"])
    else:
        w, h = {TALK: (9 + int(5 * level), 3 + int(11 * level)), DIZZY: (8, 6)}.get(expr, (7, 7))
        ellipse(mx, my + 1, w, h, COLORS["mouth"])
    return big.resize(img.size, Image.LANCZOS)


def load(assets):
    with open(os.path.join(assets, "anchors.json")) as f:
        meta = json.load(f)
    frame = lambda i: Image.open(os.path.join(assets, "frames", f"f_{i:03d}.jpg")).convert("RGB")
    return meta, frame


def make_gif(assets, clips, out, scale):
    meta, frame = load(assets)
    table = {c["name"]: c for c in meta["clips"]}
    images, durations = [], []
    for name in clips:
        c = table[name]
        for k in range(c["count"]):
            i = c["first"] + k
            img = draw_face(frame(i), meta["frames"][i], spin=(k * 72) % 360, eye_size=meta["rest_eye_size"])
            images.append(img.resize((img.width * scale // 2, img.height * scale // 2), Image.LANCZOS))
            durations.append(1000 // c["fps"])
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    images[0].save(out, save_all=True, append_images=images[1:], duration=durations, loop=0, optimize=True)
    print(f"wrote {out} ({len(images)} frames)")


def make_sheet(assets, out):
    meta, frame = load(assets)
    table = {c["name"]: c for c in meta["clips"]}
    first = lambda name, k=0: table[name]["first"] + k
    cases = [  # (frame, expression override, gaze, mic level, label)
        (first("idle"), None, (0, 0), 0, "idle"), (first("idle"), None, (1, -0.6), 0, "eyes follow"),
        (first("wave", 15), None, (0, 0), 0, "wave"), (first("giggle", 6), None, (0, 0), 0, "giggle"),
        (first("cheer", 6), None, (0, 0), 0, "cheer"), (first("lean", 0), None, (-1, 0.3), 0, "lean"),
        (first("dizzy", 8), None, (0, 0), 0, "dizzy"), (first("listen", 7), TALK, (0.1, -1), 0.6, "listen"),
        (first("squeeze", 7), None, (0, 0), 0, "squeeze"), (first("lean", 11), SURPRISED, (1, -0.2), 0, "swipe"),
    ]
    tiles = []
    for i, expr, gaze, level, _ in cases:
        img = draw_face(frame(i), meta["frames"][i], expr, gaze, level, 60, meta["rest_eye_size"])
        tiles.append(img.crop((84, 56, 306, 226)).resize((333, 255), Image.LANCZOS))
    cols = 5
    sheet = Image.new("RGB", (333 * cols, 255 * ((len(tiles) + cols - 1) // cols)))
    for k, t in enumerate(tiles):
        sheet.paste(t, ((k % cols) * 333, (k // cols) * 255))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    sheet.save(out)
    print(f"wrote {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--assets", default="assets")
    ap.add_argument("--clips", nargs="*", help="clip names (default: all)")
    ap.add_argument("--out", help="output file")
    ap.add_argument("--scale", type=int, default=2, help="GIF size as a multiple of half resolution")
    ap.add_argument("--sheet", action="store_true", help="write an expression sheet PNG instead of a GIF")
    args = ap.parse_args()
    if args.sheet:
        make_sheet(args.assets, args.out or "build/preview/expressions.png")
    else:
        meta, _ = load(args.assets)
        clips = args.clips or [c["name"] for c in meta["clips"]]
        make_gif(args.assets, clips, args.out or "build/preview/muse_all.gif", args.scale)


if __name__ == "__main__":
    main()
