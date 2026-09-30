#!/usr/bin/env bash
# Builds the downloadable Khonsu packages (no source code inside):
#   khonsu-<v>-linux-x86_64.tar.gz   the CLI binary (C++ runtime and OpenSSL linked statically),
#                                    both SDKs and a quickstart
#   khonsu-<v>-py3-none-any.whl      the Python SDK      khonsu-<v>.tgz   the JavaScript SDK
#   SHA256SUMS, downloads.json (read by the website's Downloads page), install.sh
#
#   scripts/package.sh OUT_DIR SITE_URL      e.g. scripts/package.sh web/public/downloads https://you.github.io/Khonsu
set -euo pipefail
out="$(realpath -m "${1:?usage: package.sh OUT_DIR SITE_URL}")"
site="${2:?usage: package.sh OUT_DIR SITE_URL}"
site="${site%/}"
root="$(cd "$(dirname "$0")/.." && pwd)"
py="${PYTHON:-python3}"
build="$root/build/package"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
cd "$root"

# ---- the binary ---------------------------------------------------------------------------
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="${CXX:-g++}" \
  -DKHONSU_BUILD_TESTS=OFF -DKHONSU_BUILD_BENCH=OFF -DOPENSSL_USE_STATIC_LIBS=TRUE \
  -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections" \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc -Wl,--gc-sections -Wl,-O1" >/dev/null
cmake --build "$build" --target khonsu >/dev/null
bin="$build/khonsu"
strip --strip-all "$bin"
version="$("$bin" version | awk '{print $2}')"
arch="$(uname -m)"
[ "$arch" = "x86_64" ] || { echo "package.sh: expected x86_64, got $arch" >&2; exit 1; }

# The binary must depend only on the C library family, which every Linux has.
if ldd "$bin" | grep -vE 'linux-vdso|libc\.so|libm\.so|libpthread\.so|libdl\.so|librt\.so|ld-linux|libgcc_s' | grep -q '=>'; then
  echo "package.sh: unexpected dynamic dependencies:" >&2; ldd "$bin" >&2; exit 1
fi
glibc="$(objdump -T "$bin" | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -n 1 | cut -d_ -f2)"
"$bin" version >/dev/null

# ---- the SDKs -------------------------------------------------------------------------------
mkdir -p "$stage/sdk"
# Build from a scratch copy: setuptools writes build/ and *.egg-info next to the sources, and an
# egg-info on the test PYTHONPATH would register the pytest plugin a second time.
cp -r "$root/sdk/python" "$stage/pysrc"
rm -rf "$stage/pysrc/build" "$stage"/pysrc/src/*.egg-info
"$py" -m pip wheel --quiet --no-deps --no-build-isolation -w "$stage/sdk" "$stage/pysrc" 2>/dev/null \
  || "$py" -m pip wheel --quiet --no-deps -w "$stage/sdk" "$stage/pysrc"
(cd "$root/sdk/js" && npm pack --silent --pack-destination "$stage/sdk" >/dev/null)
wheel="$(basename "$(ls "$stage"/sdk/*.whl)")"
tgz="$(basename "$(ls "$stage"/sdk/*.tgz)")"

# ---- the bundle -------------------------------------------------------------------------------
name="khonsu-$version-linux-$arch"
pkg="$stage/$name"
mkdir -p "$pkg/bin" "$pkg/sdk"
cp "$bin" "$pkg/bin/khonsu"
cp "$stage/sdk/$wheel" "$stage/sdk/$tgz" "$pkg/sdk/"
cat > "$pkg/QUICKSTART.txt" <<EOF
Khonsu $version: a time-travel debugger for AI agents
Guide: $site/#/guide

  export PATH="\$PWD/bin:\$PATH"              # or run ./install.sh from the website
  khonsu record -- python agent.py            # record a run (any OpenAI-compatible or Anthropic agent)
  khonsu replay .khonsu/runs/<run>.khn -- python agent.py     # replay it: no model, no cost
  khonsu show   .khonsu/runs/<run>.khn         # every call, tool and file change
  khonsu fork   .khonsu/runs/<run>.khn@4 -- python agent.py  # go live from call #4

Tool SDKs (wrap tools with side effects so replays don't repeat them):
  pip install sdk/$wheel
  npm install sdk/$tgz

Requires Linux x86-64 with glibc $glibc or newer (Windows: inside WSL). Run 'khonsu' for every command.
EOF
tar -C "$stage" -czf "$stage/$name.tar.gz" "$name"

mkdir -p "$out"
rm -f "$out"/khonsu-* "$out"/SHA256SUMS "$out"/downloads.json "$out"/install.sh
cp "$stage/$name.tar.gz" "$stage/sdk/$wheel" "$stage/sdk/$tgz" "$out/"
(cd "$out" && sha256sum "$name.tar.gz" "$wheel" "$tgz" > SHA256SUMS)

sed -e "s|@SITE@|$site|g" -e "s|@VERSION@|$version|g" -e "s|@ARCHIVE@|$name.tar.gz|g" \
  "$root/scripts/install.sh.in" > "$out/install.sh"

"$py" - "$out" "$version" "$glibc" "$name.tar.gz" "$wheel" "$tgz" <<'PY'
import datetime, hashlib, json, os, sys
out, version, glibc, archive, wheel, tgz = sys.argv[1:]
def entry(f, kind, label):
    p = os.path.join(out, f)
    return {"file": f, "kind": kind, "label": label, "bytes": os.path.getsize(p),
            "sha256": hashlib.sha256(open(p, "rb").read()).hexdigest()}
manifest = {
    "version": version,
    "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"),
    "glibc": glibc,
    "files": [entry(archive, "cli", "Khonsu CLI for Linux x86-64 (includes both SDKs)"),
              entry(wheel, "python", "Python SDK (wheel)"),
              entry(tgz, "js", "JavaScript / TypeScript SDK (npm tarball)")],
}
json.dump(manifest, open(os.path.join(out, "downloads.json"), "w"), indent=2)
PY
echo "packaged khonsu $version (glibc >= $glibc) into $out:"
ls -la "$out"
