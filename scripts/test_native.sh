#!/usr/bin/env bash
# Builds one native preset and runs every test suite against it. Full output of
# each suite goes to build/logs/<preset>-<suite>.log (uploaded by CI on failure);
# the console shows the pass/fail summary.
#   scripts/test_native.sh [native|native-asan|native-tsan]   (needs: clang++, ninja, python with openai + pytest)
set -uo pipefail
preset="${1:-native}"
root="$(cd "$(dirname "$0")/.." && pwd)"
py="${PYTHON:-python3}"
cd "$root"
mkdir -p build/logs

cmake --preset "$preset" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" > "build/logs/$preset-configure.log" 2>&1 || { cat "build/logs/$preset-configure.log"; exit 1; }
if ! cmake --build --preset "$preset" > "build/logs/$preset-build.log" 2>&1; then
  tail -n 60 "build/logs/$preset-build.log"
  exit 1
fi
grep -E "warning:" "build/logs/$preset-build.log" && echo "(build produced warnings)"
bin="$root/build/$preset/khonsu"

export TSAN_OPTIONS="halt_on_error=1 exitcode=66"
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}"
export PYTHONPATH="$root/sdk/python/src"
status=0
run() {
  local name="$1"; shift
  local log="build/logs/$preset-$name.log"
  echo "== $name ($preset)"
  if "$@" > "$log" 2>&1; then
    grep -E "passed|failed checks" "$log" | tail -n 1
  else
    status=1
    grep -E "FAIL|passed|failed checks|ERROR: (Address|Leak)Sanitizer|WARNING: ThreadSanitizer|runtime error|Traceback" "$log" | head -n 40
    echo "   (full log: $log)"
  fi
}

run unit "build/$preset/khonsu_tests"
run integration "$py" tests/integration/run_integration.py "$bin"
run workspace "$py" tests/integration/run_workspace.py "$bin"
run ci bash -c "cd tests/integration && '$py' run_ci.py '$bin'"
run sync bash -c "cd tests/integration && '$py' run_sync.py '$bin'"
echo "== $preset: $([ $status -eq 0 ] && echo ALL PASSED || echo FAILURES)"
exit $status
