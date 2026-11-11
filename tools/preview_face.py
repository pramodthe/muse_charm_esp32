#!/usr/bin/env python3
"""Preview the clips on your computer, with the face drawn the way the firmware
draws it, without flashing a board.

    python3 tools/preview_face.py                     # every clip -> build/preview/muse_all.gif
    python3 tools/preview_face.py --clips wave giggle # just these clips
    python3 tools/preview_face.py --sheet             # expression sheet PNG
    python3 tools/preview_face.py --shot close        # clips through a camera shot
    python3 tools/preview_face.py --shots             # the camera shots side by side

Needs Pillow (pip install pillow). The drawing is an approximation of the LVGL
face in firmware/main/face.c and the camera in firmware/main/camera.c, close
enough to check anchors, expressions and framing.
"""
import argparse
import json
import math
import os

from PIL import Image, ImageDraw

COLORS = dict(eye=(0x3A, 0x2F, 0x29), highlight=(0xFF, 0xF6, 0xEE), line=(0x4A, 0x38, 0x30),
              mouth=(0x5A, 0x2A, 0x28), tongue=(0xE0, 0x7A, 0x80))
NORMAL, HAPPY, SQUEEZE, DIZZY, TALK, SURPRISED, SLEEP = range(7)
SS = 3  # supersampling for smooth edges
WIDE = (1.0, 0.0, 0.0)  # a camera view: zoom, and the frame point at the screen's top-left


def shot_view(meta, shot, close_zoom=1.45):
    """The camera's view for a shot, as firmware/main/camera.c frames it."""
    w, h = meta["width"], meta["height"]
    idle = next(c for c in meta["clips"] if c["name"] == "idle")
    rows = meta["frames"][idle["first"]:idle["first"] + idle["count"]]
    face_x = sum((r[0] + r[2]) / 2 for r in rows) / len(rows)
    face_y = sum((r[1] + r[3]) / 2 for r in rows) / len(rows)

    def framing(zoom, fx, fy, sx, sy):
        ox = min(max(fx - sx * w / zoom, 0), w - w / zoom)
        oy = min(max(fy - sy * h / zoom, 0), h - h / zoom)
        return zoom, ox, oy

    return {
        "wide": WIDE,
        "medium": framing(1.18, face_x, face_y + 85, 0.5, 0.52),
        "close": framing(close_zoom, face_x, face_y, 0.5, 0.42),
        "peek": framing(close_zoom * 1.25, face_x, face_y + 4, 0.5, 0.45),
    }[shot]


def rotate(r, dx, dy):
    c, s = math.cos(r), math.sin(r)
    return dx * c - dy * s, dx * s + dy * c


def draw_face(img, row, expr=None, gaze=(0.0, 0.0), level=0.0, spin=0, eye_size=(10, 13), view=WIDE,
              caption=None):
    """The frame as the board shows it: through the camera `view`, with the face
    drawn on top at the view's zoom (so it stays sharp), and an optional caption."""
    zoom, vx, vy = view
    if zoom > 1.004:  # the board enlarges the body picture with a bilinear filter
        img = img.resize(img.size, Image.BILINEAR, box=(vx, vy, vx + img.width / zoom, vy + img.height / zoom))
    to_screen = lambda x, y: ((x - vx) * zoom, (y - vy) * zoom)
    elx, ely = to_screen(row[0], row[1])
    erx, ery = to_screen(row[2], row[3])
    mx, my = to_screen(row[4], row[5])
    roll, frame_expr = row[6], row[7]
    expr = frame_expr if expr is None else expr
    big = img.resize((img.width * SS, img.height * SS))
    d = ImageDraw.Draw(big)
    r = math.radians(roll)
    z = zoom

    def ellipse(cx, cy, w, h, col):
        d.ellipse([(cx - w / 2) * SS, (cy - h / 2) * SS, (cx + w / 2) * SS, (cy + h / 2) * SS], fill=col)

    def arc(cx, cy, size, a0, a1, width, col, rot=0.0):
        d.arc([(cx - size / 2) * SS, (cy - size / 2) * SS, (cx + size / 2) * SS, (cy + size / 2) * SS],
              a0 + rot, a1 + rot, fill=col, width=int(round(width * SS)))

    gx, gy = gaze[0] * 3.5 * z, gaze[1] * 2.5 * z
    for i, (ex, ey) in enumerate(((elx, ely), (erx, ery))):
        if expr == HAPPY:
            ox, oy = rotate(r, 0, 4 * z)
            arc(ex + ox, ey + oy, 13 * z, 205, 335, 3 * z, COLORS["line"], roll)
        elif expr == SLEEP:
            ox, oy = rotate(r, 0, -3 * z)
            arc(ex + ox, ey + oy, 13 * z, 25, 155, 3 * z, COLORS["line"], roll)
        elif expr == SQUEEZE:
            outer, inner = (2, 8) if i == 0 else (8, 2)
            x0, y0 = ex - 5 * z, ey - 6.5 * z
            d.line([((x0 + outer * z) * SS, (y0 + 2 * z) * SS), ((x0 + inner * z) * SS, (y0 + 6 * z) * SS),
                    ((x0 + outer * z) * SS, (y0 + 11 * z) * SS)], fill=COLORS["line"],
                   width=int(round(3 * z * SS)), joint="curve")
        elif expr == DIZZY:
            arc(ex, ey, 12 * z, 0, 290, 2 * z, COLORS["eye"], spin if i == 0 else 360 - spin)
        else:
            w, h = eye_size if expr != SURPRISED else (eye_size[0] + 2, eye_size[1] + 2)
            ellipse(ex + gx, ey + gy, w * z, h * z, COLORS["eye"])
            ellipse(ex + gx - w * z * 0.22, ey + gy - h * z * 0.25, 3 * z, 3 * z, COLORS["highlight"])
    if expr in (NORMAL, SLEEP):
        ox, oy = rotate(r, 0, -6 * z)
        arc(mx + ox, my + oy, 20 * z, 35, 145, 3 * z, COLORS["line"], roll)
    elif expr in (HAPPY, SQUEEZE):
        ox, oy = rotate(r, 0, -2.5 * z)
        d.pieslice([(mx + ox - 9 * z) * SS, (my + oy - 9 * z) * SS, (mx + ox + 9 * z) * SS, (my + oy + 9 * z) * SS],
                   roll, 180 + roll, fill=COLORS["mouth"])
        ox, oy = rotate(r, 0, 3.5 * z)
        ellipse(mx + ox, my + oy, 8 * z, 4 * z, COLORS["tongue"])
    else:
        w, h = {TALK: (9 + 8 * level, 3 + 15 * level), DIZZY: (8, 6)}.get(expr, (7, 7))
        ellipse(mx, my + z, w * z, h * z, COLORS["mouth"])
    out = big.resize(img.size, Image.LANCZOS)

    if expr == SLEEP or caption:
        font = caption_font()
        overlay = Image.new("RGBA", out.size, (0, 0, 0, 0))
        od = ImageDraw.Draw(overlay)
        if expr == SLEEP:  # "z z Z" just off the head's upper right
            ox, oy = rotate(r, 58 * z, -78 * z)
            od.text(((elx + erx) / 2 + ox, (ely + ery) / 2 + oy), "z z Z", font=font, fill=(0xD8, 0xD0, 0xC8, 178))
        if caption:  # a dark pill at the bottom, readable over the body in a close-up
            left, top, right, bottom = od.textbbox((0, 0), caption, font=font)
            tw, th = right - left, 24
            x0, y1 = (out.width - tw) / 2 - 14, out.height - 10
            od.rounded_rectangle([x0, y1 - th - 10, x0 + tw + 28, y1], radius=16, fill=(0, 0, 0, 153))
            od.text((x0 + 14 - left, y1 - th - 6), caption, font=font, fill=(0xF4, 0xEE, 0xE8, 255))
        out = Image.alpha_composite(out.convert("RGBA"), overlay).convert("RGB")
    return out


def caption_font():
    """A 20 px sans-serif close to the board's Montserrat, or Pillow's default."""
    from PIL import ImageFont
    for name in ("Montserrat-Medium.ttf", "DejaVuSans.ttf", "Arial.ttf", "Helvetica.ttc"):
        try:
            return ImageFont.truetype(name, 20)
        except OSError:
            pass
    return ImageFont.load_default()


def load(assets):
    with open(os.path.join(assets, "anchors.json")) as f:
        meta = json.load(f)
    frame = lambda i: Image.open(os.path.join(assets, "frames", f"f_{i:03d}.jpg")).convert("RGB")
    return meta, frame


def make_gif(assets, clips, out, scale, shot="wide"):
    meta, frame = load(assets)
    table = {c["name"]: c for c in meta["clips"]}
    view = shot_view(meta, shot)
    images, durations = [], []
    for name in clips:
        c = table[name]
        for k in range(c["count"]):
            i = c["first"] + k
            img = draw_face(frame(i), meta["frames"][i], spin=(k * 72) % 360, eye_size=meta["rest_eye_size"],
                            view=view)
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


def make_shots(assets, out):
    """The camera's shots as the board frames them, with the face and a caption."""
    meta, frame = load(assets)
    table = {c["name"]: c for c in meta["clips"]}
    first = lambda name, k=0: table[name]["first"] + k
    cases = [  # (frame, shot, expression, gaze, mouth level, caption)
        (first("idle"), "wide", None, (0, 0), 0, "Editing face.c"),
        (first("wave", 15), "medium", None, (0, 0), 0, None),
        (first("listen", 7), "close", NORMAL, (0, -0.15), 0, "Listening..."),
        (first("idle", 4), "close", TALK, (0, 0), 0.7, "Why did the cookie go to"),
        (first("idle", 8), "peek", SURPRISED, (0, 0), 0, None),
        (first("idle", 12), "medium", SLEEP, (0, 0), 0, None),
    ]
    tiles = [draw_face(frame(i), meta["frames"][i], expr, gaze, level, 0, meta["rest_eye_size"],
                       shot_view(meta, shot), caption) for i, shot, expr, gaze, level, caption in cases]
    w, h = tiles[0].size
    sheet = Image.new("RGB", ((w + 8) * len(tiles) - 8, h), (24, 24, 24))
    for k, t in enumerate(tiles):
        sheet.paste(t, (k * (w + 8), 0))
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
    ap.add_argument("--shot", choices=["wide", "medium", "close", "peek"], default="wide",
                    help="camera shot for the GIF (default: wide)")
    ap.add_argument("--shots", action="store_true", help="write the camera shots side by side as a PNG")
    args = ap.parse_args()
    if args.sheet:
        make_sheet(args.assets, args.out or "build/preview/expressions.png")
    elif args.shots:
        make_shots(args.assets, args.out or "build/preview/shots.png")
    else:
        meta, _ = load(args.assets)
        clips = args.clips or [c["name"] for c in meta["clips"]]
        make_gif(args.assets, clips, args.out or "build/preview/muse_all.gif", args.scale, args.shot)


if __name__ == "__main__":
    main()
