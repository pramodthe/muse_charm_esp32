#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Put the companion server online with Modal, so Muse works without this
computer.

    python3 companion/deploy_modal.py
    python3 companion/deploy_modal.py --code-only    after changing only the server's code

Run it yourself, once (and again after changing your Deepgram key or voice
options). It needs a Modal account you are logged in to (`modal setup`)
and your Deepgram key in ~/.config/muse/deepgram_key. It:

  1. makes a random token (the password Muse and the Claude Code hooks present)
     and saves it in ~/.config/muse/server.json,
  2. stores your Deepgram key and that token in Modal as the secret
     "muse-companion",
  3. deploys modal_app.py, and saves the address Modal gives it.

Afterwards: `python3 companion/muse_companion.py pair` tells Muse the address,
and `python3 companion/install_hooks.py --online` points Claude Code at it.
"""

import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
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

    print("Storing the Deepgram key and Muse's token in Modal (secret \"muse-companion\")...")
    values = ["DEEPGRAM_API_KEY=" + key, "MUSE_TOKEN=" + token]
    if os.path.exists(muse_voice.OPTIONS_FILE):  # your voice, model and personality choices go along
        with open(muse_voice.OPTIONS_FILE) as f:
            values.append("MUSE_VOICE=" + " ".join(f.read().split()))
    done = subprocess.run(modal + ["secret", "create", "--force", "muse-companion"] + values,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if done.returncode != 0:
        sys.exit("Modal did not take the secret:\n" + done.stdout.replace(key, "<key>").replace(token, "<token>"))

    deploy(modal)


def deploy(modal):
    print("Deploying the server...")
    run = subprocess.Popen(modal + ["deploy", os.path.join(HERE, "modal_app.py")], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
    output = ""
    for line in run.stdout:
        output += line
        print("  " + line.rstrip())
    if run.wait() != 0:
        sys.exit("The deployment failed; see the messages above.")
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
