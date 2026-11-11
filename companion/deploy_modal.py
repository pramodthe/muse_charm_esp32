#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Put the companion server online with Modal, so Muse works without this
computer.

    python3 companion/deploy_modal.py
    python3 companion/deploy_modal.py --code-only    after changing only the server's code

Run it yourself, once (and again after changing a key or your voice options).
It needs a Modal account you are logged in to (`modal setup`) and your keys
in the repository's .env file: DEEPGRAM_API_KEY, and ANTHROPIC_API_KEY if you
want background jobs. It:

  1. makes a random token (the password Muse and the Claude Code hooks present)
     and saves it in ~/.config/muse/server.json,
  2. stores your keys and that token in Modal as the secret "muse-companion",
  3. deploys modal_app.py, and saves the address Modal gives it.

Afterwards: `python3 companion/muse_companion.py pair` tells Muse the address,
and `python3 companion/install_hooks.py --online` points Claude Code at it.
"""

import os
import re
import shutil
import subprocess
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import muse_jobs    # noqa: E402
import muse_online  # noqa: E402
import muse_voice   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def modal_cli():
    if shutil.which("modal"):
        return ["modal"]
    if shutil.which("uvx"):
        return ["uvx", "modal"]
    sys.exit("Modal's command line tool is not installed. Install it with `pip install modal` (or install uv), "
             "log in with `modal setup`, then run this again.")


def main():
    modal = modal_cli()
    if "--code-only" in sys.argv:
        deploy(modal)
        return
    key, problem = muse_voice.key_status()
    if not key:
        sys.exit(problem)
    token = muse_online.token()

    worker_key = muse_jobs.anthropic_key()
    print("Storing the Deepgram key%s and Muse's token in Modal (secret \"muse-companion\")..."
          % (", the Anthropic key" if worker_key else ""))
    values = ["DEEPGRAM_API_KEY=" + key, "MUSE_TOKEN=" + token]
    if worker_key:
        values.append("ANTHROPIC_API_KEY=" + worker_key)
    else:
        print("  (no ANTHROPIC_API_KEY in .env: Muse will not be able to do background jobs)")
    if os.path.exists(muse_voice.OPTIONS_FILE):  # your voice, model and personality choices go along
        with open(muse_voice.OPTIONS_FILE) as f:
            values.append("MUSE_VOICE=" + " ".join(f.read().split()))
    done = subprocess.run(modal + ["secret", "create", "--force", "muse-companion"] + values,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if done.returncode != 0:
        said = done.stdout.replace(key, "<key>").replace(token, "<token>")
        sys.exit("Modal did not take the secret:\n" + (said.replace(worker_key, "<key>") if worker_key else said))

    deploy(modal)


def retire():
    """Ask the version that is running to let Muse go. Modal keeps an old
    version alive for as long as a connection to it stays open, which would
    keep the new one from starting."""
    saved = muse_online.load()
    if not saved.get("url"):
        return
    req = urllib.request.Request(muse_online.http_url(saved["url"], "/retire"), data=b"", method="POST",
                                 headers={"Authorization": "Bearer " + saved.get("token", "")})
    try:
        urllib.request.urlopen(req, timeout=15).read()
        print("Asked the running server to let Muse go; Muse reconnects to the new one by itself.")
    except OSError:
        pass  # nothing running, or a version from before this existed


def deploy(modal):
    retire()
    print("Deploying the server...")
    run = subprocess.Popen(modal + ["deploy", os.path.join(HERE, "modal_app.py")], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
    output = ""
    for line in run.stdout:
        output += line
        print("  " + line.rstrip())
    if run.wait() != 0:
        sys.exit("The deployment failed; see the messages above. The version that was running carries on, "
                 "and takes Muse back within five minutes.")
    found = re.findall(r"https://[\w.-]+\.modal\.run", output)
    if not found:
        sys.exit("Deployed, but I could not find the server's address in Modal's output. Save it yourself as "
                 "\"url\" in %s." % muse_online.SERVER_FILE)

    saved = muse_online.load()
    saved["url"] = muse_online.device_url(found[-1])
    muse_online.save(saved)
    print("\nThe server is online at %s" % found[-1])
    print("Saved in %s. Next:" % muse_online.SERVER_FILE)
    print("  python3 companion/test_voice.py --online          try a conversation through it")
    print("  python3 companion/muse_companion.py pair          tell Muse where it is (Muse on the same WiFi)")
    print("  python3 companion/install_hooks.py --online       point Claude Code at it")


if __name__ == "__main__":
    main()
