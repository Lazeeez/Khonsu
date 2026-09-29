#!/usr/bin/env bash
# Builds one native preset and runs every test suite against it.
#   scripts/test_native.sh [native|native-asan|native-tsan]   (needs: clang++, ninja, python with openai + pytest)
set -uo pipefail
preset="${1:-native}"
root="$(cd "$(dirname "$0")/.." && pwd)"
py="${PYTHON:-python3}"
cd "$root"

cmake --preset "$preset" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" >/dev/null || exit 1
cmake --build --preset "$preset" 2>&1 | grep -E "error|warning" && echo "(build produced errors/warnings)"
bin="$root/build/$preset/khonsu"
[ -x "$bin" ] || { echo "build failed"; exit 1; }

export TSAN_OPTIONS="halt_on_error=1 exitcode=66"
export PYTHONPATH="$root/sdk/python/src"
status=0
run() { echo "== $1"; shift; "$@" 2>&1 | grep -E "FAIL|passed|failed checks|WARNING: (Address|Thread|Undefined)|runtime error" || status=1; [ "${PIPESTATUS[0]}" -eq 0 ] || status=1; }

run "unit ($preset)" "build/$preset/khonsu_tests"
run "integration ($preset)" "$py" tests/integration/run_integration.py "$bin"
run "workspace ($preset)" "$py" tests/integration/run_workspace.py "$bin"
run "ci ($preset)" bash -c "cd tests/integration && '$py' run_ci.py '$bin'"
echo "== $preset: $([ $status -eq 0 ] && echo ALL PASSED || echo FAILURES)"
exit $status
