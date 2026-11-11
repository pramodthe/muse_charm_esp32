# SPDX-License-Identifier: MIT
"""The companion server on Modal (https://modal.com), so Muse works without
your computer.

    python3 companion/deploy_modal.py      sets everything up and deploys this

One small container runs muse_companion.py in online mode. Modal gives it an
https address and passes websockets through; the Deepgram key and the token
Muse must present come from the Modal secret "muse-companion". The container
runs while Muse is connected and stops a few minutes after it disconnects.
What Muse is asked to remember, and what its background jobs write, live on
the Modal volume "muse-companion-data". The image also holds the packages the
background worker needs (muse_worker.py); it works once the secret has an
ANTHROPIC_API_KEY.
The time zone is taken from the computer that deploys this.
"""

import os
import subprocess
import sys

import modal

PORT = 8765
HERE = os.path.dirname(os.path.abspath(__file__))



def local_timezone():
    """This computer's time zone as an IANA name ("" if it can't be told)."""
    if os.environ.get("TZ"):
        return os.environ["TZ"]
    path = os.path.realpath("/etc/localtime")
    return path.split("zoneinfo/", 1)[1] if "zoneinfo/" in path else ""


def local_option(name):
    """A setting from this computer's .env or environment, read when deploying."""
    try:
        sys.path.insert(0, HERE)
        import muse_env
        return muse_env.get(name)
    except ImportError:   # inside the container: the value was fixed at deploy time
        return os.environ.get(name, "")


app = modal.App("muse-companion")
image = modal.Image.debian_slim(python_version="3.12").pip_install(
    "tzdata", "deepagents", "langchain-anthropic").add_local_dir(
    HERE, "/app", ignore=["__pycache__", "*.pyc", "*.wav", ".env*"])
data = modal.Volume.from_name("muse-companion-data", create_if_missing=True)


@app.function(
    image=image,
    secrets=[
        modal.Secret.from_name("muse-companion"),  # DEEPGRAM_API_KEY, MUSE_TOKEN, ANTHROPIC_API_KEY
        modal.Secret.from_dict({"MUSE_TZ": local_timezone(), "MUSE_DATA_DIR": "/data",
                                "MUSE_RESULTS_PUBLIC": local_option("MUSE_RESULTS_PUBLIC")}),
    ],
    volumes={"/data": data},
    max_containers=1,       # Muse and the Claude Code hooks must reach the same server
    timeout=24 * 60 * 60,   # the longest one connection can last; Muse reconnects by itself
    scaledown_window=300,
)
@modal.concurrent(max_inputs=50)
@modal.web_server(PORT, startup_timeout=60)
def server():
    subprocess.Popen(["python3", "-u", "/app/muse_companion.py", "--port", str(PORT)])
