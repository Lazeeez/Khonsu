#!/usr/bin/env bash
# Cuts a release: sets the version everywhere (one commit per file), tags it, and pushes.
# The tag starts .github/workflows/release.yml, which builds, tests and publishes.
#
#   scripts/release.sh 0.3.1          a release
#   scripts/release.sh 0.4.0-rc1      a pre-release (a hyphen marks it as one)
set -euo pipefail
version="${1:?usage: scripts/release.sh VERSION   (e.g. 0.3.1)}"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] || { echo "not a version: $version" >&2; exit 1; }
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
tag="v$version"

[ "$(git rev-parse --abbrev-ref HEAD)" = "main" ] || { echo "release from main" >&2; exit 1; }
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "commit or stash your changes first" >&2; exit 1; }
git fetch --quiet --tags origin
git rev-parse -q --verify "refs/tags/$tag" >/dev/null && { echo "$tag already exists" >&2; exit 1; }
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] || { echo "main is not in sync with origin/main; pull or push first" >&2; exit 1; }

bump() {  # file, sed expression, commit message
  sed -i "$2" "$1"
  if ! git diff --quiet -- "$1"; then git commit -q -m "$3" -- "$1" && echo "✓ $1"; fi
}
bump platform/native/main.cpp "s/^constexpr const char\* kVersion = \".*\";/constexpr const char* kVersion = \"$version\";/" \
  "Set the CLI version to $version"
bump sdk/python/pyproject.toml "s/^version = \".*\"/version = \"$version\"/" "Set the Python SDK version to $version"
bump sdk/js/package.json "s/^  \"version\": \".*\",/  \"version\": \"$version\",/" "Set the JavaScript SDK version to $version"
scripts/check_version.sh "$version"

git tag -a "$tag" -m "Khonsu $version"
git push --quiet origin main "$tag"
echo "pushed $tag: the release workflow is building it now (GitHub → Actions → Release)"
