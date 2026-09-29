#!/usr/bin/env bash
# Regenerates the sample recordings shipped with the website (web/public/samples/).
# They are produced by the real khonsu binary recording real agents (the OpenAI
# SDK) against the test model server, so the viewer demonstrates exactly what a
# user's own recordings look like.
#   scripts/make_samples.sh [path/to/khonsu]      (needs python with openai)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
k="${1:-$root/build/native/khonsu}"
py="${PYTHON:-python3}"
out="$root/web/public/samples"
tmp="$(mktemp -d)"
trap 'kill $mock 2>/dev/null || true; rm -rf "$tmp"' EXIT
mkdir -p "$out"
rm -f "$out"/*.khn

port=$("$py" -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
"$py" "$root/tests/integration/mock_llm.py" "$port" > "$tmp/mock.log" &
mock=$!
sleep 1
up="http://127.0.0.1:$port/v1"
export PYTHONPATH="$root/sdk/python/src" SIDE_EFFECTS="$tmp/side.log"

# A small project for the coding agent to work in.
ws="$tmp/moonlight-service"
mkdir -p "$ws/.git" "$ws/src" "$ws/tests" "$ws/docs"
printf 'Add a greeting endpoint that returns a haiku about the moon god Khonsu.\n' > "$ws/task.txt"
cat > "$ws/README.md" <<'EOF'
# moonlight-service
A tiny service used to demo Khonsu. The agent edits `out/` while it works.
EOF
cat > "$ws/src/app.py" <<'EOF'
from http.server import BaseHTTPRequestHandler

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"ok")
EOF
cat > "$ws/tests/test_app.py" <<'EOF'
def test_placeholder():
    assert True
EOF
printf '# API\n\n`GET /greet` returns a haiku.\n' > "$ws/docs/API.md"

run() {  # run <output.khn> <agent.py> [extra khonsu args...]
  local o="$1" agent="$2"; shift 2
  (cd "$ws" && "$k" record -q -o "$o" --upstream "$up" "$@" -- "$py" "$root/tests/integration/$agent" > /dev/null)
  rm -rf "$ws/out"
}

run "$out/coding-agent.khn" coder_agent.py
run "$out/coding-agent-rerun.khn" coder_agent.py
(cd "$tmp" && "$k" fork "$out/coding-agent.khn@4" --dir "$tmp/fork" -o "$out/coding-agent-fork.khn" -q \
   --upstream "$up" -- "$py" "$root/tests/integration/coder_agent.py" > /dev/null)
(cd "$tmp" && AGENT_PARALLEL=3 "$k" record -q --no-workspace -o "$out/weather-agent.khn" --upstream "$up" \
   -- "$py" "$root/tests/integration/agent.py" > /dev/null)

for f in "$out"/*.khn; do "$k" verify "$f" > /dev/null && echo "sample $(basename "$f"): $(stat -c %s "$f") bytes, verified"; done
