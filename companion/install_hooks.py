#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Add (or remove) the Claude Code hooks that tell Muse what Claude is doing.

    python3 install_hooks.py            add the hooks to ~/.claude/settings.json
    python3 install_hooks.py --remove   take them out again
    python3 install_hooks.py --print    show the hooks without changing anything

Each hook sends the event to the Muse companion server on this computer with
curl, waits at most one second, and always succeeds, so Claude Code works the
same whether or not the server or Muse is running. A backup of the settings
file is kept next to it as settings.json.muse-backup.
"""

import argparse
import json
import os
import shutil
import sys

MARK = "muse-companion"
URL = "http://127.0.0.1:8765/hook?src=" + MARK
COMMAND = ("curl -s -m 1 -o /dev/null -H 'Content-Type: application/json' -H 'Expect:' "
           "--data-binary @- '%s' || true" % URL)
EVENTS = [
    "SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PostToolUseFailure",
    "PermissionRequest", "Notification", "SubagentStart", "SubagentStop", "Stop",
    "StopFailure", "PreCompact", "SessionEnd",
]


def ours(handler):
    return isinstance(handler, dict) and MARK in str(handler.get("command", ""))


def remove(settings):
    hooks = settings.get("hooks")
    if not isinstance(hooks, dict):
        return 0
    removed = 0
    for event in list(hooks):
        groups = hooks[event]
        if not isinstance(groups, list):
            continue
        kept = []
        for group in groups:
            handlers = group.get("hooks", []) if isinstance(group, dict) else []
            left = [h for h in handlers if not ours(h)]
            removed += len(handlers) - len(left)
            if left or not handlers:
                if handlers:
                    group = dict(group, hooks=left)
                kept.append(group)
        if kept:
            hooks[event] = kept
        else:
            del hooks[event]
    if not hooks:
        del settings["hooks"]
    return removed


def add(settings):
    hooks = settings.setdefault("hooks", {})
    for event in EVENTS:
        hooks.setdefault(event, []).append({"hooks": [{"type": "command", "command": COMMAND, "timeout": 5}]})


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--remove", action="store_true", help="remove the Muse hooks")
    ap.add_argument("--print", action="store_true", help="print the hooks and change nothing")
    ap.add_argument("--settings", default=os.path.expanduser("~/.claude/settings.json"))
    args = ap.parse_args()

    if args.print:
        demo = {}
        add(demo)
        print(json.dumps(demo, indent=2))
        return

    path = args.settings
    settings = {}
    if os.path.exists(path):
        with open(path) as f:
            text = f.read()
        try:
            settings = json.loads(text) if text.strip() else {}
        except ValueError as e:
            sys.exit("%s is not valid JSON (%s); fix it first, nothing was changed" % (path, e))
        if not isinstance(settings, dict):
            sys.exit("%s is not a JSON object; nothing was changed" % path)
        shutil.copy2(path, path + ".muse-backup")

    removed = remove(settings)  # never add twice
    if not args.remove:
        add(settings)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(settings, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)

    if args.remove:
        print("Removed %d Muse hook(s) from %s" % (removed, path))
    else:
        print("Muse hooks are in %s (%d events)." % (path, len(EVENTS)))
        print("New Claude Code sessions will report to Muse; restart any that are open.")


if __name__ == "__main__":
    main()
