#!/bin/sh
# Serves this copy of the FM-4 webapp on http://localhost and opens it in Chrome.
# Web MIDI needs a secure context; localhost counts as one. Files are sent with
# "Cache-Control: no-store" so the browser never runs an older copy, and a busy
# port (an older copy still running) is skipped instead of silently reused.
cd "$(dirname "$0")"
exec python3 - <<'PY'
import http.server, socket, subprocess, sys
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()
for port in range(8041, 8060):
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
        break
    except OSError:
        continue
else:
    sys.exit("no free port between 8041 and 8059")
url = f"http://localhost:{port}"
print(f"FM-4 webapp: {url}  (close this window to stop)", flush=True)
subprocess.Popen(["open", "-a", "Google Chrome", url])
srv.serve_forever()
PY
