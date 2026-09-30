# Releasing Khonsu

A release is one command. Everything after it is automatic.

```sh
scripts/release.sh 0.3.1        # a release
scripts/release.sh 0.4.0-rc1    # a pre-release (a hyphen marks it as one)
```

The script sets the version in the three places that carry it (`platform/native/main.cpp`, `sdk/python/pyproject.toml`, `sdk/js/package.json`, one commit each), tags `v0.3.1`, and pushes. Run it on an up-to-date, clean `main`.

## What the tag does

The tag starts **Actions → Release** (`.github/workflows/release.yml`):

1. **Checks the versions.** The tag, the CLI, the Python SDK and the JavaScript SDK must all agree (`scripts/check_version.sh`).
2. **Builds and tests the package**, exactly as every push does (`.github/workflows/package.yml`):
   - a portable Linux x86-64 binary, with the C++ runtime and OpenSSL linked statically and symbols stripped;
   - both SDKs, checksums and the installer;
   - then the tests: no source paths in the binary, a checksum-verified install, and the integration suite run against the installed copy.
3. **Creates the GitHub Release**, with the downloads attached, install instructions, and notes GitHub writes from the commits since the previous tag. Pre-releases are marked as such.
4. **Publishes the Python SDK to PyPI** as [`khonsu-sdk`](https://pypi.org/project/khonsu-sdk/) (imported as `khonsu`) (once set up, below).
5. **Publishes the JavaScript SDK to npm** as [`khonsu-sdk`](https://www.npmjs.com/package/khonsu-sdk) (once set up, below). Pre-releases go under the `next` dist-tag, so `npm install khonsu-sdk` keeps giving the latest stable version.

Steps 4 and 5 skip themselves with a notice until they are configured, so the GitHub Release always works. Publishing a version twice is skipped, not an error. The website's Downloads page is rebuilt from `main` on every push. It switches to the short `pip install khonsu-sdk` and `npm install khonsu-sdk` commands automatically once those versions are live.

Releases are visible to everyone with access to this repository. The public downloads are on the website: <https://lazeeez.github.io/Khonsu/#/downloads>.

## One-time setup: PyPI

PyPI uses *trusted publishing*: GitHub proves to PyPI which workflow is publishing, so no password or token is stored anywhere.

1. Create an account at <https://pypi.org> and turn on two-factor authentication.
2. Go to **Your account → Publishing → Add a new pending publisher → GitHub**:
   - PyPI project name: `khonsu-sdk`
   - Owner: `Lazeeez`
   - Repository name: `Khonsu`
   - Workflow name: `release.yml`
   - Environment name: `pypi`
3. In this repository: **Settings → Secrets and variables → Actions → Variables → New repository variable**: `PUBLISH_PYPI` = `true`.

The next release creates the project on PyPI and publishes it.

## One-time setup: npm

1. Create an account at <https://www.npmjs.com> and turn on two-factor authentication.
2. **Access Tokens → Generate New Token → Granular Access Token**:
   - Packages and scopes: **Read and write**, all packages (the package doesn't exist until the first publish). After the first release you can narrow it to `khonsu-sdk`.
   - Allow it to publish without two-factor prompts, so automation can use it.
   - Expiration: the longest npm allows for a token that can publish (90 days). Put a reminder in your calendar to replace it before then: generate a new token and update the `NPM_TOKEN` secret.
3. In this repository: **Settings → Secrets and variables → Actions → Secrets → New repository secret**: `NPM_TOKEN` = the token.

## Finishing or repairing a release

Every step is safe to repeat. If a release job fails (a flaky network, a registry outage, a secret added late), there are two ways to finish it:

- **Actions → Release → Run workflow**, enter the tag (for example `v0.3.0`), and run. This uses the *current* workflow on the tagged code. An existing GitHub Release gets its files refreshed, and versions already on PyPI or npm are skipped. Use this when the workflow itself was fixed after the tag was made.
- **Re-run failed jobs** on the original run. This uses the workflow exactly as it was when the tag was made.

To redo a release from scratch, delete the GitHub Release and the tag (`git push --delete origin v0.3.1`, `git tag -d v0.3.1`), fix the problem, and run `scripts/release.sh` again. Versions already on PyPI or npm cannot be replaced, only superseded, so a broken published version means releasing the next patch version.
