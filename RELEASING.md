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
5. **Publishes the JavaScript SDK to npm** as [`khonsu-sdk`](https://www.npmjs.com/package/khonsu-sdk) (trusted publishing, set up below). Pre-releases go under the `next` dist-tag, so `npm install khonsu-sdk` keeps giving the latest stable version.

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

npm also uses *trusted publishing*, so no token is stored and nothing expires. It can only be set up on a package that already exists; `khonsu-sdk` does.

1. Sign in at <https://www.npmjs.com> (two-factor authentication on) and open the [`khonsu-sdk`](https://www.npmjs.com/package/khonsu-sdk) package.
2. **Settings → Trusted Publisher → GitHub Actions**:
   - Organization or user: `Lazeeez`
   - Repository: `Khonsu`
   - Workflow filename: `release.yml`
   - Environment name: `npm`

   Then **Set up connection**.
3. Same page, **Publishing access**: choose **Require two-factor authentication and disallow tokens**. From then on, only this workflow (and you, with 2FA) can publish.
4. In this repository: **Settings → Secrets and variables → Actions → Variables → New repository variable**: `PUBLISH_NPM` = `true`.

Trusted publishing needs npm 11.5.1 or newer; the workflow installs it. Because this repository is private, npm doesn't attach provenance attestations (it only does that for public source), but the publish itself is authenticated the same way.

## Finishing or repairing a release

Every step is safe to repeat. If a release job fails (a flaky network, a registry outage, a secret added late), there are two ways to finish it:

- **Actions → Release → Run workflow**, enter the tag (for example `v0.3.0`), and run. This uses the *current* workflow on the tagged code. An existing GitHub Release gets its files refreshed, and versions already on PyPI or npm are skipped. Use this when the workflow itself was fixed after the tag was made.
- **Re-run failed jobs** on the original run. This uses the workflow exactly as it was when the tag was made.

To redo a release from scratch, delete the GitHub Release and the tag (`git push --delete origin v0.3.1`, `git tag -d v0.3.1`), fix the problem, and run `scripts/release.sh` again. Versions already on PyPI or npm cannot be replaced, only superseded, so a broken published version means releasing the next patch version.
