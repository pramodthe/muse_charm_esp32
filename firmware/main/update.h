// SPDX-License-Identifier: MIT
// Firmware updates over WiFi.
//
// The online companion server keeps the firmware image Muse should be running
// ("muse_companion.py update" puts a build there). When Muse links to it, Muse
// says which image it is running; if the server has another one, it offers it
// ({"type":"firmware","id":...,"size":...}). Once Muse has been left alone for
// a little while it downloads the image from the same server over HTTPS, with
// its token, into the app slot that is not in use, and restarts into it. The
// screen holds still meanwhile, with "Updating..." up.
//
// The new image is on trial until Muse has linked to a server with it. If it
// crashes first, or cannot link within ten minutes, the bootloader goes back
// to the image from before, and Muse does not install that update again.
//
// Only an online server reached over TLS (wss://) is trusted with this; a
// server found on the local network is not.
#pragma once

#include <stdbool.h>

// Call once at start-up, before companion_start().
void update_start(void);

// Which image is running: the SHA-256 at the end of the image, in hex (the
// server's "id" for it), and its version. "" before update_start().
const char *update_running_id(void);
const char *update_running_version(void);

// From companion.c, a few times a second: keep or drop an image on trial, and
// start an update that was offered once Muse has been left alone.
void update_poll(void);

// From companion.c: the online server has this image for Muse.
void update_offer(const char *id, int size);

// From companion.c: Muse has linked to a server, so this firmware works.
void update_on_linked(void);

// An update is being downloaded: Muse does not start a conversation now.
bool update_busy(void);
