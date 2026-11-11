# SPDX-License-Identifier: MIT
"""The companion server on Modal (https://modal.com), so Muse works without
your computer.

    python3 companion/deploy_modal.py      sets everything up and deploys this

One small container runs muse_companion.py in online mode. Modal gives it an
https address and passes websockets through; the Deepgram key and the token
Muse must present come from the Modal secret "muse-companion". The container
runs while Muse is connected and stops a few minutes after it disconnects.
"""

import os
import subprocess

import modal

PORT = 8765
HERE = os.path.dirname(os.path.abspath(__file__))

app = modal.App("muse-companion")
image = modal.Image.debian_slim(python_version="3.12").add_local_dir(
    HERE, "/app", ignore=["__pycache__", "*.pyc", "*.wav"])


@app.function(
    image=image,
    secrets=[modal.Secret.from_name("muse-companion")],  # DEEPGRAM_API_KEY and MUSE_TOKEN
    max_containers=1,       # Muse and the Claude Code hooks must reach the same server
    timeout=24 * 60 * 60,   # the longest one connection can last; Muse reconnects by itself
    scaledown_window=300,
)
@modal.concurrent(max_inputs=50)
@modal.web_server(PORT, startup_timeout=60)
def server():
    subprocess.Popen(["python3", "-u", "/app/muse_companion.py", "--port", str(PORT)])
