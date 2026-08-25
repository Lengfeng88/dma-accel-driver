# Track C — C5 Completion Note

**Status: DONE**
**Date verified:** 2026-08-20

## What was added

Purely additive to C0–C4 — no existing file modified:

```
track-c/runtime/dma_accel_buffer_pool.hpp
track-c/runtime/dma_accel_buffer_pool.cpp
track-c/tests/dma_accel_buffer_pool_smoke_test.cpp
```

## Scope correction found before implementation (not an oversight — a
real ABI constraint)

C5's originally-stated scope (pool/arena, alignment, fragmentation
handling) partially doesn't apply given M10's actual `Buffer` design:
there is no `BUFFER_FREE` ioctl — a `buffer_id`, once allocated, stays
allocated until the owning `Stream`'s fd closes. This removes:

- **Alignment** — `Buffer`'s mapping is `mmap`-backed, already
  page-aligned by construction; nothing for a pool to manage.
- **Fragmentation** — no `free()` exists to create holes, and this isn't
  a variable-size allocator; reuse is fixed-size slot cycling, not
  general allocation.

What's real and demand-driven: Track C's own C3 testing directly hit
Track B's M12 per-session buffer quota (16 buffers/session) because every
test allocated fresh buffers for every operation, never reusing any. That
observed pressure is C5's actual justification.

## Design recap

- `BufferPool`: pre-allocates `count` same-size buffers once, at
  construction — the only place it ever calls `Stream::alloc_buffer()`.
  Non-copyable AND non-movable (not just non-copyable) — `PooledBuffer`
  holds pointers into the pool's own storage; ruled out entirely rather
  than reasoned about case-by-case.
- `PooledBuffer`: RAII handle, returns its buffer to the pool's free list
  automatically on destruction.
- `acquire()` returns `std::optional<PooledBuffer>`, not an exception, on
  exhaustion — matches `WaitResult::Pending`'s precedent from C2: an
  ordinary, expected runtime condition, not a caller mistake.
- **Documented, not silently glossed over**: the pool does not protect
  against releasing a buffer while a command referencing it is still in
  flight — that's a real, callable data race if a `PooledBuffer` is
  dropped before its associated `Fence` is ready. This is the caller's
  responsibility, same non-owning-lifetime discipline used throughout
  Track C (Buffer/CommandGroup/PendingCommand's existing conventions).

## Build

Compiles clean (zero warnings) on the target guest VM (`ai-accel`),
alongside the unmodified C0–C4 files.

## Runtime verification

Run on `ai-accel`:

```
== dma-accel C5 BufferPool smoke test ==
using device /dev/dma_accel0 (each scenario opens its own Stream/session)
-- 1. checkout / exhaustion --
PASS: pool correctly exhausted after capacity acquires, further acquire() is nullopt
-- 2. release returns the SAME buffer_id --
PASS: released buffer correctly reused (buffer_id=0 both times)
-- 3. real COPY through pooled buffers, correct release discipline --
PASS: COPY through pooled buffers completed correctly, released only after Fence ready
-- 4. quota-saving: many cycles through a pool of 2 --
PASS: 5 cycles of acquire/submit/wait/release through a pool of 2 used only 2 distinct buffer_ids total (not 10) — this is the actual quota pressure C5 exists to relieve
PASS: BufferPool verified — checkout/exhaustion, real reuse, correct release discipline, and quota-saving all correct
```

Scenario 4 is the concrete proof of value: 5 full acquire/submit/wait/
release cycles through a 2-buffer pool consumed exactly 2 distinct
`buffer_id`s total, not 10 — directly demonstrating relief of the same
quota pressure C3's testing actually hit.

## KCTL note

Consistent with the frozen §6.5 decision: C5 remains single-threaded,
single-Stream, explicitly-driven (no background reclamation, no
concurrent acquire/release). No `kctl` concurrency primitives were
evaluated or needed here — that evaluation is deferred to C6, where
genuine concurrency first appears.

## Open item carried over

Same as C0–C4: Track A/B remain unsplit in `~/dma-accel-driver/` root.
Decision still deferred.

## Result

C5 marked DONE. Proceed to C6 (Multi-stream & Overlap) — the first
milestone with genuine concurrency, and the first real KCTL evaluation
point per §6.5.
