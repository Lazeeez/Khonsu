#!/usr/bin/env bash
# Checks that the CLI, the Python SDK and the JavaScript SDK all carry one version,
# and (if given) that it is the expected one:   scripts/check_version.sh [0.3.1]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cli="$(sed -n 's/^constexpr const char\* kVersion = "\(.*\)";/\1/p' "$root/platform/native/main.cpp")"
py="$(sed -n 's/^version = "\(.*\)"/\1/p' "$root/sdk/python/pyproject.toml")"
js="$(sed -n 's/^  "version": "\(.*\)",/\1/p' "$root/sdk/js/package.json")"
echo "CLI $cli · Python SDK $py · JavaScript SDK $js"
[ -n "$cli" ] && [ "$cli" = "$py" ] && [ "$cli" = "$js" ] || { echo "versions disagree" >&2; exit 1; }
if [ $# -gt 0 ] && [ "$1" != "$cli" ]; then
  echo "expected version $1, the code says $cli (run scripts/release.sh $1)" >&2
  exit 1
fi
