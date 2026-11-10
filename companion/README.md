# Claude Code companion

Muse sits next to your keyboard and shows what Claude Code is doing on your computer:

| Claude Code | Muse |
|---|---|
| A session starts | Waves, shows the project name |
| Thinking | Glances up and aside, "Thinking..." |
| Running a tool | Reads the caption: "Editing face.c", "Run the tests" |
| Waiting for your permission | Waves for attention (up to three times): "OK to flash the firmware?" |
| A tool call fails | A quiet, puzzled look |
| Finished its reply | Cheers: "Done!" |
| The turn failed (API error) | Shakes its head |
| Compacting its memory | Squeezes: "Tidying up memory" |
| The session ends | Waves goodbye |

With several sessions open, Muse shows the one that most needs you.

## How it works

```
Claude Code ──hooks (curl)──> muse_companion.py ──websocket──> Muse
             127.0.0.1:8765     on your computer    over WiFi
```

- Each Claude Code hook sends its event to the server on your computer. The hooks wait at most
  one second and always succeed, so Claude Code behaves the same whether the server is running
  or not.
- The server turns the events into a mood and a short caption. It announces itself on the local
  network with a UDP beacon (port 47800), so Muse finds it without any setup.
- Prompts, file contents and commands stay on your computer. Muse only receives the mood, a short
  description such as "Editing face.c" or the one-line description Claude writes for a shell
  command, and the project folder name.

## Set up

You need Python 3.8 or later and curl (both come with macOS). No packages to install.

1. Put Muse on the same WiFi network as your computer (setup opens on Muse's screen).
2. Start the server and leave it running:

   ```sh
   python3 companion/muse_companion.py
   ```

   Muse giggles and shows "Linked to ..." when it finds the server.

   On macOS with the firewall on, Muse can only connect if the firewall lets Python accept
   incoming connections. Apple's own `/usr/bin/python3` is allowed by default; a Homebrew or
   python.org Python needs approving in System Settings → Network → Firewall → Options (and again
   after each Python upgrade). If Muse finds the server but never links, start it with
   `/usr/bin/python3 companion/muse_companion.py`.

3. Add the hooks to Claude Code (your other settings and hooks are kept, and a backup is saved
   as `~/.claude/settings.json.muse-backup`):

   ```sh
   python3 companion/install_hooks.py
   ```

   New Claude Code sessions report to Muse; restart sessions that are already open.

Try it without Claude Code: with the server running, `python3 companion/muse_companion.py demo`
plays a made-up session. `curl http://127.0.0.1:8765/` shows what the server sees.

To take the hooks out again: `python3 companion/install_hooks.py --remove`.

## Troubleshooting

- **Muse finds the server but never links** (the serial log shows "Error read response for
  Upgrade header"): the computer's firewall is blocking it; see step 2 above.
- **Muse never finds the server.** Some networks (guest WiFi, many offices) block broadcasts or traffic
  between devices. Set the server address by hand in `idf.py menuconfig` → Muse charm → Claude
  Code companion, for example `ws://192.168.1.20:8765/device`.
- **The server logs nothing when Claude works.** Check that `~/.claude/settings.json` has the
  hooks (`install_hooks.py --print` shows what they look like) and that the session was started
  after installing them.
- **Muse stays on "Thinking..."** after you interrupt Claude: interrupted turns send no "done"
  event, so the mood clears by itself after five minutes, or at your next prompt.

The companion talks to Muse over plain websocket on your local network, without pairing. Anyone
on the same network could send Muse captions; use it on networks you trust.
