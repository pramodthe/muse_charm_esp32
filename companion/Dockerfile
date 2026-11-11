# The companion server for any host that runs containers (DigitalOcean App
# Platform, Fly.io, Render, a VPS ...). The host must pass websockets through
# and serve it over https. Set these in the host's settings, never in this file:
#
#   DEEPGRAM_API_KEY   your Deepgram key
#   MUSE_TOKEN         a long random password; Muse and the hooks present it
#   PORT               the port to listen on, if the host chooses it (default 8765)
#
#   docker build -t muse-companion companion && docker run -p 8765:8765 \
#       -e DEEPGRAM_API_KEY=... -e MUSE_TOKEN=... muse-companion
FROM python:3.12-slim
WORKDIR /app
COPY muse_companion.py muse_voice.py muse_ws.py muse_online.py ./
EXPOSE 8765
CMD ["python3", "-u", "muse_companion.py"]
