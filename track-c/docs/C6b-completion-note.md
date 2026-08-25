# Track C — C6b Completion Note

**Status: DONE**
**Date verified:** 2026-08-21

## What was added

```
track-c/tests/dma_accel_multistream_threaded_test.cpp
track-c/third_party/kctl/  (kctl headers, vendored — not modified)
```

## First real KCTL integration (per §6.5)

This is the first Track C milestone to use `kctl`. The evaluation was
done component-by-component, not "does KCTL fit somewhere":

- **`kctl::MpmcBoundedQueue`** — used. Genuine fit: multiple worker
  threads (each exclusively driving one `Stream`, per M10's single-
  thread-per-Stream invariant) need to report completion events to one
  aggregator without a mutex. This is exactly the multi-producer
  scenario the queue is for.
- **`kctl::ThreadPoolMpmc` / `ThreadPoolSharded`** — evaluated, rejected.
  Both model workers pulling one-shot tasks from a queue; C6b's threads
  are persistent, dedicated Stream drivers (submit-then-poll-repeatedly
  for the thread's whole lifetime), not one-shot task executors. Forcing
  that shape would be adopting KCTL machinery to serve an architecture
  diagram — exactly what §6.5 warns against.
- **`kctl::DeviceHandle`** — evaluated, rejected. `Stream` already
  RAII-owns its own fd (frozen Track A code); `DeviceHandle` requires an
  `IDeviceBackend` abstraction `Stream` has no way to plug into without
  modifying it.

## Design recap

Two real OS threads, each opening and exclusively driving its own
`Stream` (never shared across threads), submitting commands as fast as
possible with **no artificial coordination** between them — unlike C6a,
where a single thread manually alternated submission order (A0, B0, A1,
B1...) to directly control SQ ring position. C6b asks a different
question: does genuine concurrent, uncoordinated multi-threaded
submission naturally produce overlapping dispatch on its own?

Each thread reports a `CompletionEvent` (stream id, cmd_id, elapsed time)
into a shared `kctl::MpmcBoundedQueue` the moment it observes one of its
own `Fence`s become ready. The main thread joins both workers, drains the
queue, sorts by timestamp, and reports the same "transitions" metric used
in C6a.

**Exception-safety bug found and fixed during verification** (not a
design flaw, a real correctness gap in the first draft): an exception
thrown inside a `std::thread`'s function body never reaches the creating
thread's `try`/`catch` — it calls `std::terminate()` immediately
regardless of what `main()` does. The original version had the device-
open (`Stream` constructor) inside the worker thread with no local
`try`/`catch`, so a missing `/dev/dma_accel0` (driver not loaded — an
environment issue, confirmed separately) caused an unhandled exception
inside the thread and the whole process aborted with an unhelpful
`terminate called recursively` message instead of a clean error. Fixed
by wrapping each worker's body in its own `try`/`catch`, capturing into a
`std::exception_ptr` that the main thread checks and rethrows after
joining.

## Runtime verification

Run on `ai-accel`, 2 threads x 8 commands each:

```
== dma-accel C6b concurrent multi-thread submission test ==
using device /dev/dma_accel0, 2 threads, 8 commands each, KCTL MpmcBoundedQueue for completion reporting
-- completion timeline (reconstructed from KCTL MPMC queue) --
     50173 us  stream=1 cmd=0x2
     50668 us  stream=0 cmd=0x5
     51345 us  stream=1 cmd=0x3
     51350 us  stream=1 cmd=0x4
    100749 us  stream=1 cmd=0x6
    100754 us  stream=1 cmd=0x7
    100756 us  stream=1 cmd=0x9
    100777 us  stream=0 cmd=0x8
    149869 us  stream=0 cmd=0xa
    151013 us  stream=0 cmd=0xc
    151159 us  stream=1 cmd=0xb
    151164 us  stream=1 cmd=0xd
    199768 us  stream=0 cmd=0xe
    200913 us  stream=0 cmd=0xf
    200919 us  stream=0 cmd=0x10
    200922 us  stream=0 cmd=0x11
-- summary --
total completions: 16, cross-stream transitions: 5
RESULT: genuine interleaving emerged from real concurrent multi-threaded submission alone, no manual alternation needed here — a stronger result than C6a's engineered version.
PASS: C6b coordinator ran two real OS threads, each exclusively driving its own Stream, reporting completions through KCTL's lock-free MpmcBoundedQueue with no data races and no mutex
```

5 transitions across 16 completions — genuine, naturally-occurring
cross-thread interleaving, not perfectly alternating (some clustering:
stream 1 dominant early, stream 0 dominant late, mixed in the middle) but
clearly not the strict block pattern C6a's first (block-submission)
attempt showed. A positive, real finding: with real concurrent threads,
uncoordinated submission does produce some natural interleaving, though
C6a's manually-controlled version (7 transitions) still achieved more
deliberate, even interleaving — consistent with C6a's conclusion that
deliberate interleaving (C7's eventual job) is what actually maximizes
overlap, while naive concurrency alone provides a real but weaker effect.

## KCTL note

C6b closes out the §6.5 evaluation with a concrete, working, correctly-
justified integration: one KCTL component used for a real reason
(`MpmcBoundedQueue`), two evaluated and explicitly rejected with stated
reasons (`ThreadPoolMpmc`/`Sharded`, `DeviceHandle`). This is the pattern
§6.5 asked for — KCTL earning its place through demonstrated need, not
retrofitted for architectural completeness.

## Open item carried over

Same as C0-C6a: Track A/B remain unsplit in `~/dma-accel-driver/` root.
Decision still deferred.

## Result

C6b marked DONE. C6 (Multi-stream & Concurrent Execution) is now
complete — C6a established the mechanism (dispatch is FIFO-by-ring-
position, no session awareness), C6b established genuine multi-threaded
execution with a real, justified KCTL integration. Proceed to C7
(Execution Scheduling), now directly informed by both C6a and C6b's
findings.
