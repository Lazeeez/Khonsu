# ADR 0005: Workspace snapshots are Merkle trees stored inside the recording

**Status:** accepted · 2026-09-29

## Context

Model calls are only half of an agent run. Coding agents edit files, run tests and read
the results, so to reproduce or fork a run at call #N we need **the files as they were
at call #N**. Options:

| Option | Problem |
|---|---|
| Copy the directory at every call | O(repo) per call; a 40 MiB repo × 200 calls = 8 GiB |
| `git stash`/commits in the user's repo | Mutates the user's repository; fails outside git |
| A separate object store (`~/.khonsu/objects`) | Recordings are no longer self-contained: can't attach one to a bug report or open it in a browser |

## Decision

- Before every request (model or tool), and at run start and end, the workspace is
  captured as a **content-addressed Merkle tree**: blobs for file contents, one tree
  object per directory, ids = XXH3-128 of the bytes (git's model, different hash).
- Objects are appended to the **recording itself** as `Object` frames, each id at most
  once per recording. A `Snapshot` frame records the root id. The `.khn` file stays
  self-contained: `checkout`, `cat` and the browser viewer need nothing else. Object frames
  are self-verifying (the id is recomputed on load).
- Unchanged files are not re-read: a stat cache keyed by (size, mtime, inode, device), with
  git's racy-file rule (files modified in the last 2 s are always re-hashed).
- Ignore rules follow **gitignore semantics** (nested files, negation, anchoring, directory-only,
  `**`), plus `.git/info/exclude` and `.khonsuignore`. **Secret-bearing files are excluded by
  default** (`.env*`, `*.pem`, `*.key`, SSH keys, `.netrc`, credential files), because
  recordings are meant to be shared.
- Files over 8 MiB are tracked by a streamed content hash (changes are still detected) but
  not stored.

## Consequences

- A new snapshot costs O(changed files) in storage and O(files) in `stat` calls.
- Diffs between any two points in a run descend only into subtrees whose ids differ.
- Replay compares the live workspace to the recorded snapshot before every call and names
  the files that differ (`+scratch.txt`), catching nondeterministic tools as well as prompts.

## Evidence

Measured on Linux (ext4), 12-thread laptop:

| Workspace | First snapshot | Each later snapshot | One-file edit adds |
|---|---|---|---|
| Khonsu repo (77 files after .gitignore; node_modules skipped) | 2.4 ms | 0.3 ms | n/a |
| Synthetic, 20,000 files / 40 MiB | 146 ms | 34 ms | 4 objects, 10 KiB (the file + 3 trees on its path) |

`tests/integration/run_workspace.py` checks gitignore semantics, secret exclusion, `cat` and
`checkout` at arbitrary points, restorability of every snapshot (`verify`), and replay-time
drift detection.
