#!/bin/sh
# Serves the FM-4 webapp on http://localhost:8040 and opens it in Chrome.
# Web MIDI needs a secure context; localhost counts as one.
cd "$(dirname "$0")"
PORT=8040
( sleep 1; open -a "Google Chrome" "http://localhost:$PORT" 2>/dev/null || open "http://localhost:$PORT" ) &
echo "FM-4 webapp: http://localhost:$PORT  (close this window to stop)"
exec python3 -m http.server "$PORT" --bind 127.0.0.1
