#!/usr/bin/env bash
# Encode rendered PNG frames into the baseline JPEGs the firmware embeds.
#
#   tools/encode_frames.sh [png_dir] [out_dir]
#
# Defaults: build/render -> assets/frames. Needs ffmpeg.
# blender/export_clips.py runs this step itself when ffmpeg is on PATH.
set -euo pipefail

PNG_DIR="${1:-build/render}"
OUT_DIR="${2:-assets/frames}"

command -v ffmpeg >/dev/null || { echo "ffmpeg is required (brew install ffmpeg / apt install ffmpeg)" >&2; exit 1; }
[ -f "$PNG_DIR/f_0001.png" ] || { echo "no frames in $PNG_DIR (expected f_0001.png ...)" >&2; exit 1; }

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/f_*.jpg
# 4:2:0 baseline JPEG at quality 3 keeps each frame around 10 KB
ffmpeg -loglevel error -y -start_number 1 -i "$PNG_DIR/f_%04d.png" \
    -q:v 3 -pix_fmt yuvj420p -start_number 0 "$OUT_DIR/f_%03d.jpg"
echo "encoded $(ls "$OUT_DIR"/f_*.jpg | wc -l | tr -d ' ') frames into $OUT_DIR"
