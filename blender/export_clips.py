"""Export Muse's reaction clips for the device.

Builds every clip on the rig, writes the per-frame face anchors to
assets/anchors.json and renders the body frames (with the face hidden, since
the firmware draws it live). If ffmpeg is on PATH the frames are also encoded
to the baseline JPEGs the firmware embeds (assets/frames/f_000.jpg ...).

Run from the repository root:

    blender -b blender/muse.blend --python blender/export_clips.py -- --out assets

Another .blend of the same character and rig works in place of muse.blend,
including one that still has the older object names (see ALIASES).

Options (after the "--"):
    --out DIR          where anchors.json and frames/ go (default: assets)
    --png-dir DIR      where the PNG renders go (default: build/render)
    --anchors-only     skip rendering, only write anchors.json
    --save             save the generated "Muse Clips (device)" action into the .blend

Edit the clip functions below to change a motion, or add one to CLIPS and to
firmware/main/behavior.c.
"""

import argparse
import json
import math
import os
import shutil
import subprocess
import sys

import bpy
from bpy_extras.object_utils import world_to_camera_view
from mathutils import Euler, Quaternion, Vector

WIDTH, HEIGHT = 368, 448
RIG = "Muse Rig"
CAMERA = "Camera Device 368x448"
WAVE_SOURCE_ACTION = "Muse Wave (source)"
CLIPS_ACTION = "Muse Clips (device)"
EYE_L, EYE_R, MOUTH = "Eye L", "Eye R", "Mouth Smile"
# Face parts the firmware draws itself, hidden in the renders
FACE_PARTS = [EYE_L, EYE_R, "Eye Highlight L", "Eye Highlight R", "Eye Happy L", "Eye Happy R", MOUTH, "Mouth Open"]
REST_EYE_SIZE = [10, 13]  # pixels, drawn by the firmware

# Names the same things have in older versions of the model
ALIASES = {
    RIG: ["V5 | MUSE grounded rig"],
    CAMERA: ["V11 | DEVICE 368x448"],
    WAVE_SOURCE_ACTION: ["Muse | wave hello 10s"],
    EYE_L: ["V6 | glossy dot eye L"],
    EYE_R: ["V6 | glossy dot eye R"],
    "Eye Highlight L": ["V6 | eye reflection L"],
    "Eye Highlight R": ["V6 | eye reflection R"],
    "Eye Happy L": ["V5 | happy closed eye L"],
    "Eye Happy R": ["V5 | happy closed eye R"],
    MOUTH: ["V5 | small curved smile"],
    "Mouth Open": ["V8 | open happy mouth"],
}


def find(collection, name):
    """The object or action called `name`, or what an older model calls it."""
    for candidate in [name] + ALIASES.get(name, []):
        if candidate in collection:
            return collection[candidate]
    sys.exit(f"'{name}' is missing from the .blend (also tried: {', '.join(ALIASES.get(name, [])) or 'nothing'})")

# Expressions the clips can request per frame (see firmware/main/muse.h)
NORMAL, HAPPY, SQUEEZE, DIZZY = 0, 1, 2, 3
# Clip flags read by the firmware
LOOP, INDEXED, HOLD_LAST = 1, 2, 4

# ---------------------------------------------------------------------------
# Rig posing
# ---------------------------------------------------------------------------
ARM_REST_ANGLE = 120.0  # angle of the arm bones in the rest pose
REST_ARM = (36.0, 28.0, 20.0, 0.0)  # upper arm, forearm, hand, forward swing
# The standing pose, in the rig's own space: how high the pelvis is and how far
# out the feet are. The rig is moved there from its rest pose, wherever that is.
STANCE_PELVIS_Z = 0.875
STANCE_FOOT_X = 0.514


def _conj(rig, bone, rotation):
    m = rig.data.bones[bone].matrix_local.to_quaternion()
    return (m.inverted() @ rotation @ m).normalized()


def arm_quats(rig, side, upper, fore, hand, swing=0.0):
    """Arm angles in degrees from straight down, outward positive."""
    s = 1 if side == "L" else -1
    ry = lambda a: Quaternion((0, 1, 0), math.radians(s * a))
    rz = lambda a: Quaternion((0, 0, 1), math.radians(s * a))
    return {
        "upper_arm." + side: _conj(rig, "upper_arm." + side, rz(swing) @ ry(upper - ARM_REST_ANGLE)),
        "forearm." + side: _conj(rig, "forearm." + side, ry(fore - upper)),
        "hand." + side: _conj(rig, "hand." + side, ry(hand - fore)),
    }


def body_quats(rig, spine, head, pelvis):
    """(pitch, lean, yaw) in degrees for each bone."""
    e = lambda v: Euler([math.radians(x) for x in v], "XYZ").to_quaternion()
    return {"spine": _conj(rig, "spine", e(spine)), "head": _conj(rig, "head", e(head)),
            "pelvis": _conj(rig, "pelvis", e(pelvis))}


def reset(rig):
    for pb in rig.pose.bones:
        pb.rotation_mode = "QUATERNION"
        pb.rotation_quaternion = (1, 0, 0, 0)
        pb.location = (0, 0, 0)
        pb.scale = (1, 1, 1)


def base_pose():
    return dict(spine=[0, 0, 0], head=[0, 0, 0], pelvis=[0, 0, 0], sx=1.0, sz=1.0, px=0.0, pz=0.0, hop=0.0,
                L=list(REST_ARM), R=list(REST_ARM), expr=NORMAL)


def apply_pose(rig, p):
    reset(rig)
    q = body_quats(rig, p["spine"], p["head"], p["pelvis"])
    q.update(arm_quats(rig, "L", *p["L"]))
    q.update(arm_quats(rig, "R", *p["R"]))
    for name, value in q.items():
        rig.pose.bones[name].rotation_quaternion = value
    rig.pose.bones["spine"].scale = (p["sx"], p["sz"], p["sx"])
    pelvis = rig.data.bones["pelvis"]
    mp = pelvis.matrix_local.to_3x3().inverted()
    stance_z = STANCE_PELVIS_Z - pelvis.head_local.z
    rig.pose.bones["pelvis"].location = mp @ Vector((p["px"], 0, stance_z + p["pz"] + p["hop"]))
    for side in "LR":
        foot = rig.data.bones["foot." + side]
        mf = foot.matrix_local.to_3x3().inverted()
        x = foot.head_local.x
        rig.pose.bones["foot." + side].location = mf @ Vector((math.copysign(STANCE_FOOT_X, x) - x, 0, p["hop"]))


def snapshot(rig):
    return {pb.name: (tuple(pb.rotation_quaternion), tuple(pb.location), tuple(pb.scale)) for pb in rig.pose.bones}


def restore(rig, snap):
    for name, (q, loc, scale) in snap.items():
        pb = rig.pose.bones[name]
        pb.rotation_mode = "QUATERNION"
        pb.rotation_quaternion = q
        pb.location = loc
        pb.scale = scale


# ---------------------------------------------------------------------------
# Clips: each function returns the pose for frame i of n
# ---------------------------------------------------------------------------
def sstep(t, a, b):
    x = min(1.0, max(0.0, (t - a) / (b - a)))
    return x * x * (3 - 2 * x)


def idle(i, n):
    """Seamless breathing loop."""
    p = base_pose()
    w = 2 * math.pi * i / n
    b = math.sin(w)
    p["sx"], p["sz"], p["pz"] = 1 + 0.010 * b, 1 + 0.005 * b, -0.004 * b
    p["spine"] = [0.4 * math.sin(2 * w), 1.2 * math.sin(w + 0.5), 0.8 * math.sin(w + 1.4)]
    p["head"] = [0.8 * math.sin(2 * w + 0.3), 1.6 * math.sin(w + 1.2), 2.0 * math.sin(w + 2.0)]
    p["L"] = [36 + 1.5 * math.sin(w + 0.8), 28 + 2.0 * math.sin(w + 0.5), 20 + 2.5 * math.sin(w + 0.2), 0]
    p["R"] = [36 + 1.5 * math.sin(w + 1.8), 28 + 2.0 * math.sin(w + 1.5), 20 + 2.5 * math.sin(w + 1.2), 0]
    return p


def giggle(i, n):
    """Scrunched-up shaking laugh."""
    p = base_pose()
    e = sstep(i, 0, 3) * (1 - sstep(i, n - 4, n - 1))
    ph = 2 * math.pi * i / 4.0
    s, c = math.sin(ph), 0.5 + 0.5 * math.cos(ph)
    p["spine"] = [1.5 * e, 5 * e * s, 0]
    p["head"] = [-6 * e + 1.5 * e * c, -3 * e * math.sin(ph + 0.6), 2 * e * s]
    p["sx"], p["sz"], p["pz"] = 1 + 0.03 * e * c, 1 - 0.035 * e * c, -0.012 * e * c
    for k, sg in (("L", 1), ("R", -1)):
        p[k] = [36 - 2 * e + 4 * e * s * sg, 28 - 23 * e + 6 * e * s * sg, 20 - 35 * e + 8 * e * s * sg, 45 * e]
    p["expr"] = HAPPY if 1 <= i <= n - 2 else NORMAL
    return p


def cheer(i, n):
    """Crouch, hop with both arms up, land."""
    p = base_pose()
    crouch = sstep(i, 0, 3) * (1 - sstep(i, 3, 5)) + 0.8 * sstep(i, 9, 10) * (1 - sstep(i, 10, 13))
    air = sstep(i, 3, 6) * (1 - sstep(i, 7, 10))
    up = sstep(i, 2, 6) * (1 - sstep(i, 13, 19))
    wig = math.sin(2 * math.pi * i / 3.0) * up
    p["pz"], p["hop"] = -0.05 * crouch, 0.16 * air
    p["sx"], p["sz"] = 1 + 0.04 * crouch - 0.02 * air, 1 - 0.06 * crouch + 0.04 * air
    p["spine"] = [2 * crouch - 2 * air, 0, 0]
    p["head"] = [-6 * up + 3 * crouch, 2 * wig, 0]
    for k, sg in (("L", 1), ("R", -1)):
        p[k] = [36 + 92 * up, 28 + 108 * up + 6 * wig * sg, 20 + 122 * up + 10 * wig * sg, 10 * up]
    p["expr"] = HAPPY if 3 <= i <= 16 else NORMAL
    return p


def lean(i, n):
    """Static poses from -18 to +18 degrees, picked by the tilt angle."""
    a = -18 + 36 * i / (n - 1)
    p = base_pose()
    p["pelvis"], p["spine"], p["head"] = [0, 0.45 * a, 0], [0, 0.35 * a, 0], [0, 0.25 * a, -0.15 * a]
    p["px"] = 0.002 * a
    p["L"] = [max(26, 36 + 0.35 * a), max(14, 28 + 0.45 * a), max(4, 20 + 0.55 * a), 0]
    p["R"] = [max(26, 36 - 0.35 * a), max(14, 28 - 0.45 * a), max(4, 20 - 0.55 * a), 0]
    return p


def dizzy(i, n):
    """Circular wobble that settles."""
    p = base_pose()
    e = sstep(i, 0, 3) * (1 - sstep(i, n - 8, n - 1))
    ph = 2 * math.pi * i / 8.0
    amp = 7 * e
    p["spine"] = [0.6 * amp * math.cos(ph), amp * math.sin(ph), 0]
    p["head"] = [0.5 * amp * math.cos(ph - 0.9), 0.8 * amp * math.sin(ph - 0.9), 2 * e * math.sin(ph * 0.5)]
    p["pz"], p["px"] = -0.01 * e, 0.006 * e * math.sin(ph)
    p["L"] = [36 + 8 * e * math.sin(ph + 1.2), 28 + 12 * e * math.sin(ph + 0.8), 20 + 16 * e * math.sin(ph + 0.4), 0]
    p["R"] = [36 - 8 * e * math.sin(ph + 1.2), 28 - 12 * e * math.sin(ph + 0.8), 20 - 16 * e * math.sin(ph + 0.4), 0]
    p["expr"] = DIZZY if 2 <= i <= n - 4 else NORMAL
    return p


def listen(i, n):
    """Curious head tilt, held while you talk."""
    p = base_pose()
    e = sstep(i, 0, n - 1)
    p["spine"], p["head"] = [-1.5 * e, -2 * e, 0], [-6 * e, -9 * e, -4 * e]
    p["L"] = p["R"] = [36 + 4 * e, 28 + 8 * e, 20 + 12 * e, 0]
    p["sx"], p["sz"] = 1 - 0.01 * e, 1 + 0.015 * e
    return p


def squeeze(i, n):
    """Squash with arms tucked in, held while pressed."""
    p = base_pose()
    e = sstep(i, 0, n - 1)
    p["sx"], p["sz"], p["pz"] = 1 + 0.06 * e, 1 - 0.08 * e, -0.03 * e
    p["spine"], p["head"] = [1.0 * e, 0, 0], [-4 * e, 0, 0]
    p["L"] = p["R"] = [36 - 2 * e, 28 - 23 * e, 20 - 35 * e, 45 * e]
    p["expr"] = SQUEEZE if i >= 2 else NORMAL
    return p


def wave_times(n):
    """The wave reuses the hand-tuned source action (24 fps), resampled to
    10 fps: the raise and first waves (frames 26-67), then the last waves and
    the lowering (frames 155-199)."""
    return [26 + 2.4 * i if i < 18 else 155 + 2.4 * (i - 18) for i in range(n)]


def wave_expr(i, n):
    return HAPPY if 4 <= i <= 33 else NORMAL


# name, frame count, pose function (None = sampled from the source action), fps, flags
CLIPS = [
    ("idle", 30, idle, 10, LOOP),
    ("wave", 37, None, 10, 0),
    ("giggle", 16, giggle, 10, 0),
    ("cheer", 20, cheer, 10, 0),
    ("lean", 13, lean, 10, INDEXED),
    ("dizzy", 24, dizzy, 10, 0),
    ("listen", 8, listen, 10, HOLD_LAST),
    ("squeeze", 8, squeeze, 12, HOLD_LAST),
]


# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------
def to_pixels(scene, camera, world_point):
    v = world_to_camera_view(scene, camera, world_point)
    return v.x * WIDTH, (1 - v.y) * HEIGHT


def screen_centre(scene, name):
    """Centre of an object's on-screen bounding box, from its evaluated mesh."""
    obj = find(bpy.data.objects, name)
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    pts = [to_pixels(scene, scene.camera, obj.matrix_world @ v.co) for v in mesh.vertices]
    evaluated.to_mesh_clear()
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2


def build_clips(scene, rig):
    """Pose every frame of every clip; returns (pose snapshots, expressions, clip table)."""
    source = find(bpy.data.actions, WAVE_SOURCE_ACTION)
    rig.animation_data_create()
    snaps, exprs, table = [], [], []
    for name, count, fn, fps, flags in CLIPS:
        table.append(dict(name=name, first=len(snaps), count=count, fps=fps, flags=flags))
        for i in range(count):
            if fn is None:
                t = wave_times(count)[i]
                rig.animation_data.action = source
                scene.frame_set(int(t), subframe=t - int(t))
                snap = snapshot(rig)
                rig.animation_data.action = None
                expr = wave_expr(i, count)
            else:
                rig.animation_data.action = None
                p = fn(i, count)
                apply_pose(rig, p)
                snap = snapshot(rig)
                expr = p["expr"]
            snaps.append(snap)
            exprs.append(expr)
    return snaps, exprs, table


def key_clips(scene, rig, snaps):
    """Key the snapshots as one action, one frame each, starting at frame 1."""
    old = bpy.data.actions.get(CLIPS_ACTION)
    if old is not None:
        bpy.data.actions.remove(old)
    action = bpy.data.actions.new(CLIPS_ACTION)
    rig.animation_data.action = action
    for index, snap in enumerate(snaps):
        frame = index + 1
        scene.frame_set(frame)
        restore(rig, snap)
        for pb in rig.pose.bones:
            for path in ("rotation_quaternion", "location", "scale"):
                pb.keyframe_insert(path, frame=frame, group=pb.name)
    scene.frame_start, scene.frame_end = 1, len(snaps)
    scene.render.fps = 10
    return action


def export_anchors(scene, rig, exprs, table, out_dir):
    rows = []
    for index, expr in enumerate(exprs):
        scene.frame_set(index + 1)
        el, er, m = screen_centre(scene, EYE_L), screen_centre(scene, EYE_R), screen_centre(scene, MOUTH)
        roll = math.degrees(math.atan2(er[1] - el[1], er[0] - el[0]))
        rows.append([round(v, 1) for v in (*el, *er, *m, roll)] + [expr])
    scene.frame_set(1)
    reset(rig)
    rig.animation_data.action = None
    apply_pose(rig, base_pose())
    bpy.context.view_layer.update()
    belly = to_pixels(scene, scene.camera, rig.matrix_world @ rig.pose.bones["spine"].head)
    data = dict(width=WIDTH, height=HEIGHT, clips=table, frames=rows,
                belly=[round(belly[0]), round(belly[1])], rest_eye_size=REST_EYE_SIZE)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "anchors.json")
    with open(path, "w") as f:
        json.dump(data, f, indent=1)
    print(f"wrote {path}: {len(rows)} frames in {len(table)} clips")


def render_frames(scene, png_dir):
    os.makedirs(png_dir, exist_ok=True)
    hidden = {}
    for name in FACE_PARTS:
        obj = find(bpy.data.objects, name)
        hidden[obj] = obj.hide_render
        obj.hide_render = True
    scene.render.resolution_x, scene.render.resolution_y = WIDTH, HEIGHT
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.filepath = os.path.join(png_dir, "f_")
    bpy.ops.render.render(animation=True)
    for obj, value in hidden.items():
        obj.hide_render = value


def encode_frames(png_dir, out_dir):
    ffmpeg = shutil.which("ffmpeg")
    frames_dir = os.path.join(out_dir, "frames")
    if not ffmpeg:
        print(f"ffmpeg not found: encode the PNGs in {png_dir} with tools/encode_frames.sh")
        return
    os.makedirs(frames_dir, exist_ok=True)
    for old in os.listdir(frames_dir):
        if old.endswith(".jpg"):
            os.remove(os.path.join(frames_dir, old))
    subprocess.run([ffmpeg, "-loglevel", "error", "-y", "-start_number", "1", "-i", os.path.join(png_dir, "f_%04d.png"),
                    "-q:v", "3", "-pix_fmt", "yuvj420p", "-start_number", "0",
                    os.path.join(frames_dir, "f_%03d.jpg")], check=True)
    print(f"encoded frames into {frames_dir}")


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description="Export Muse's clips for the device")
    parser.add_argument("--out", default="assets")
    parser.add_argument("--png-dir", default=os.path.join("build", "render"))
    parser.add_argument("--anchors-only", action="store_true")
    parser.add_argument("--save", action="store_true")
    args = parser.parse_args(argv)
    out_dir, png_dir = os.path.abspath(args.out), os.path.abspath(args.png_dir)

    scene = bpy.context.scene
    scene.camera = find(bpy.data.objects, CAMERA)
    rig = find(bpy.data.objects, RIG)

    snaps, exprs, table = build_clips(scene, rig)
    key_clips(scene, rig, snaps)
    export_anchors(scene, rig, exprs, table, out_dir)
    rig.animation_data.action = bpy.data.actions[CLIPS_ACTION]
    scene.frame_set(1)
    if not args.anchors_only:
        render_frames(scene, png_dir)
        encode_frames(png_dir, out_dir)
    if args.save:
        bpy.ops.wm.save_mainfile()


if __name__ == "__main__":
    main()
