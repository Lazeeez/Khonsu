# ADR 0009: Lune, a deterministic VM for forking running programs

**Status:** accepted · 2026-09-30

## Context

Recording forks an agent *between* model calls. The showcase goal was to fork a program
*while it runs*, at any instruction, with both copies continuing, and to do it in the
browser on the same persistent store the recorder uses. That requires three things:

1. **Complete state.** Everything that determines the future must be data we control.
   A process in a real OS (registers, heap, file descriptors, the clock) is not, and neither
   is a JavaScript or WebAssembly guest in the browser.
2. **Determinism.** The same state must always produce the same future, or "fork" and
   "replay" mean nothing.
3. **Cheap forks of large state.** A program with megabytes of arrays must fork in
   microseconds, and a fork must not copy what it doesn't change.

## Decision

Build **Lune**: a small stack VM with its own language, kept in the kernel
(`kernel/src/lune.cpp`) and compiled natively and to WebAssembly.

- **Language:** `let`, assignment, `while`, `if/else`, `print`, 64-bit integers with wrapping
  arithmetic, `array(n)`, `len`, indexing, and `hash(x)` (splitmix64). `hash` is the only
  source of "randomness", and it is deterministic, so speculation varies an explicit seed. No
  floats, clock, I/O or threads, so there is nothing nondeterministic to capture.
- **State in the Merkle store.** `commit()` writes pc, stack, variables, output and the source
  as blobs, and each array as a *File* of 4 KiB pages: the same persistent HAMT, the same
  canonical Merkle digests and the same mark/sweep GC as workspace snapshots.
- **A mutable working set.** A running VM does not path-copy on every store. It keeps dirty
  pages in a hash map and a clean-page read cache, with a one-entry "last page" cache in front
  (a loop touches 512 consecutive values per page). `commit()` is O(dirty pages).
- **fork() = commit() + restore(root).** The child starts with no pages in memory and reads them
  lazily from the store. Its first write to a page copies that page (copy-on-write). A fork of
  an already-committed VM is a root copy plus decoding a few small blobs: about 3 µs in the
  browser, independent of array size.
- **Gas.** Every instruction costs one unit, which bounds a run and makes "step N" well
  defined, so a fork point is an instruction count.
- **Its own store in the browser.** The Play page uses a separate `NodeStore`, so the simulator's
  GC can never free a VM's pages. GC roots are every live VM's last commit and its fork point.

## Alternatives considered

- **Fork a WebAssembly or JS guest.** Linear memory can be snapshotted, but not incrementally,
  and not deterministically across host calls. Copying 8 MiB of linear memory per fork is
  exactly the O(bytes) fork this project exists to avoid.
- **Persistent structures on every write.** Path-copying the HAMT on every array store costs
  microseconds per store. A working set that commits in batches keeps the interpreter fast and
  gives the same sharing at fork boundaries.
- **An existing small language (Lua, a Scheme).** Their heaps are graphs of pointers, which
  makes page-level sharing and a canonical digest much harder. Lune's state is flat by design.

## Consequences

- Programs are limited to integers and arrays. That is enough for the showcase (ledgers,
  Monte Carlo, sieves, gamblers) and keeps the state model honest.
- A fork's cost is visible and explainable: a first fork pays for the dirty pages, later forks
  are O(1), and the page map shows exactly which pages a branch copied.

## Evidence

`tests/test_lune.cpp` (native, ASan, TSan and wasm):

- forking at 12 random instructions reproduces the uninterrupted output in both parent and child;
- `load(commit())` resumes the same computation;
- a what-if edit diverges only the fork;
- after a fork, one write copies exactly one page and the other 195 stay physically shared;
- GC keeps committed states runnable.

The E2E test forks the ledger mid-run in the browser and checks that the parent and child
finish identically.
