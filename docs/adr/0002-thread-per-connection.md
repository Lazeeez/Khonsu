# ADR 0002: One thread per connection, not an event loop

**Status:** accepted · 2026-09-29

## Context

The proxy sits between an agent and a model API. The obvious "systems" answer is an
epoll/kqueue reactor. But the workload is unusual:

- **Concurrency is tiny.** A single agent keeps 1 to about 20 connections open (parallel tool
  calls, multi-agent fan-out).
- **Latency is dominated upstream.** A model call takes 300 ms to 60 s; the proxy's own
  work (hashing a request, writing two frames, one `fdatasync`) is microseconds to
  milliseconds.
- **Streams are long-lived.** A token stream holds a connection open for seconds.

## Decision

Blocking sockets, one thread per accepted connection, all bound to 127.0.0.1. Shared state
(the replay index, occurrence counters, the recording writer) is guarded by mutexes. The
listener is shut down in two phases, `shutdown()` to wake `accept()` and `close()` after
the accept thread has joined.

## Consequences

- The streaming relay is a straight loop (read chunk, record timing, write chunk), which is
  easy to reason about and to make durable (ADR 0004).
- TLS is a blocking `SSL_read`/`SSL_write` wrapper; there is no non-blocking TLS state machine.
- Not suitable for thousands of concurrent connections. That is a non-goal: Khonsu is a
  per-developer, per-CI-job tool, not a shared gateway.

## Evidence

- ThreadSanitizer runs the integration suite (including 6 concurrent requests per run) in
  CI. It found a real race: the old `Listener::close()` wrote the fd while `accept()` read
  it, and closing an fd another thread is blocked on can race with fd-number reuse. The
  two-phase shutdown above is the fix.
