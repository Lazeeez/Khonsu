# ADR 0008: Share recordings by Merkle anti-entropy; replicate refs and notes as CRDTs

**Status:** accepted · 2026-09-30

## Context

Recordings need to move between a developer's laptop, CI, and teammates, and they should
support shared notes ("root cause at call #12"). Constraints:

- **No central server is required.** A shared drive, a CI cache directory, or one small HTTP
  process must all work.
- **Transfers must be proportional to what changed.** A fork of a run shares almost all of its
  workspace with the original.
- **Concurrent edits must never conflict or be lost.** Two people annotate the same run
  offline, then sync in any order.

## Decision

**Storage.** A store is a directory of content-addressed objects. `pack()` splits a recording
into a manifest, chunks of ordinary frames, and the workspace blobs and trees under their own
content ids. Workspace content shared between runs is therefore stored and sent once.
`unpack()` rebuilds the file byte for byte, so its hash chain still verifies. Every object is
re-verified against its id whenever it is read or received, so corruption never spreads.

**Sync (anti-entropy).** The two sides exchange a 256-bucket Merkle summary (bucket = first byte
of the id). Equal roots mean the sync is done in one round trip. Otherwise they list ids only for
the differing buckets (one batched round trip) and copy the missing objects each way. The replicated
state is merged **after** the objects, so no ref ever names an object the other side lacks. The
same algorithm runs over a directory or over HTTP (`khonsu store serve`, optional bearer token).

**Replicated state (CRDTs), merged with a join.**
- **refs** (name → manifest): a last-writer-wins map ordered by hybrid logical clocks, ties broken
  by node id, with tombstones so deletes propagate. HLCs keep causally later writes winning even
  when wall clocks are skewed.
- **annotations**: an observed-remove set. Every add carries a unique `(node, counter)` tag, and a
  remove deletes only tags it has seen, so an add made concurrently with a remove survives. An LWW
  set would silently drop it.

## Consequences

- Any topology works (star, peer-to-peer, via CI) because merges are commutative, associative and
  idempotent: gossip in any order converges.
- Deletes are tombstones and are never garbage-collected (acceptable at team scale; GC would need
  a causal-stability protocol).
- Bucket id lists are exchanged whole. At very large stores a second Merkle level would cut that
  further; not needed yet.

## Evidence

- **Unit (`tests/test_sync.cpp`)**: byte-identical pack/unpack; cross-recording dedup; forged objects
  rejected; the Merkle summary counts every id (a regression test for a real bug: sorting ids by the
  low word while walking buckets by the high byte silently skipped most objects; the sync integration
  test caught it); convergence under random operations and merge orders across 3 replicas × 60 trials;
  OR-Set add-wins; HLC LWW beating a skewed wall clock.
- **Integration (`tests/integration/run_sync.py`)**:

| Measurement | Result |
|---|---|
| Two runs sharing a 1.3 MB workspace, stored | 1,338 KiB for 2,669 KiB of files (corpus stored once) |
| Pushing a fork of an already-shared run | **4 KiB sent** for a 1.37 MB recording |
| Syncing already-equal stores | 1 root comparison, 0 objects moved |
| Round trip over directory and over HTTP | byte-identical, verifies |
| Concurrent notes on two machines, gossip in arbitrary order | identical sets |
| Wrong token / corrupted remote object | rejected / pull stops, nothing stored |
