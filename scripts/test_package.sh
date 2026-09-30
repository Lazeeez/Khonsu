#!/usr/bin/env bash
# Checks the downloadable package end to end: the binary contains no source paths, the
# installer verifies checksums (and refuses a tampered archive), and the installed CLI passes
# the integration suite.   scripts/test_package.sh   (needs: python with openai, node/npm, curl)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
py="${PYTHON:-python3}"
cd "$root"
tmp="$(mktemp -d)"
port="${PORT:-8765}"
site="http://127.0.0.1:$port"
mkdir -p "$tmp/site/downloads"
scripts/package.sh "$tmp/site/downloads" "$site" >/dev/null
archive="$(cd "$tmp/site/downloads" && ls khonsu-*-linux-*.tar.gz)"

echo "== no source code in the binary"
leaks="$(strings build/package/khonsu | grep -E "$root|/home/|\.cpp\b|\.hpp\b|kernel/src|platform/native" || true)"
[ -z "$leaks" ] || { echo "binary mentions source paths:"; echo "$leaks" | head; exit 1; }
echo "ok"

(cd "$tmp/site" && "$py" -m http.server "$port" --bind 127.0.0.1 >/dev/null 2>&1) & srv=$!
trap 'kill $srv 2>/dev/null || true; rm -rf "$tmp"' EXIT
for _ in $(seq 20); do curl -sf "$site/downloads/SHA256SUMS" >/dev/null && break; sleep 0.25; done

echo "== install"
curl -fsSL "$site/downloads/install.sh" | KHONSU_PREFIX="$tmp/prefix" sh
"$tmp/prefix/bin/khonsu" version

echo "== a tampered archive is refused"
cp "$tmp/site/downloads/$archive" "$tmp/orig"
printf 'x' >> "$tmp/site/downloads/$archive"
out="$(curl -fsSL "$site/downloads/install.sh" | KHONSU_PREFIX="$tmp/prefix2" sh 2>&1 || true)"
printf '%s' "$out" | grep -q "checksum mismatch" || { echo "tampering was not detected: $out"; exit 1; }
cp "$tmp/orig" "$tmp/site/downloads/$archive"
echo "ok"

echo "== the installed CLI and SDK pass the integration suite"
# A throwaway venv: the wheel must not leak into the environment the other suites use.
"$py" -m venv "$tmp/venv"
"$tmp/venv/bin/python" -m pip install --quiet openai "$tmp"/prefix/lib/khonsu/sdk/*.whl
"$tmp/venv/bin/python" tests/integration/run_integration.py "$tmp/prefix/bin/khonsu" | tail -n 1
