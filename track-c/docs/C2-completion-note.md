# Track C — C2 Completion Note

**Status: DONE**
**Date verified:** 2026-08-20

## What was added

Purely additive to C0/C1 — no existing file modified:

```
track-c/runtime/dma_accel_stream_observer.hpp
track-c/runtime/dma_accel_stream_observer.cpp
track-c/tests/dma_accel_stream_observer_smoke_test.cpp
```

## Design recap

`StreamObserver`: a non-owning adapter over `Stream`'s existing public
API (`pump()`, `wait(Fence&, int)`) that adds `query()` (non-blocking)
and `wait(Fence&, milliseconds)` (timeout-bounded). Exists specifically
to demonstrate Track C's composition-over-modification rule: `Stream`
and `Fence` are unmodified; if `StreamObserver` were deleted, C0/C1 are
unaffected.

Key decisions locked during design, both later confirmed against real
device behavior:

- **`Fence` stays passive** — no `Stream*` back-reference, no
  `Fence::query()`/`Fence::wait()`. `is_ready()` remains local/cached-
  state-only; `StreamObserver::query()` is the one that may actually
  pump.
- **`StreamObserver` is deliberately not move-only** — unlike
  `Buffer`/`Fence`/`Stream`/`CommandGroup`, it owns nothing and has no
  identity to protect; default copy/move used as-is.
- **`WaitResult` has exactly two values, `Completed`/`Pending`, no
  `Error`** — verified by inspecting M10's actual `Stream::wait()`
  implementation: `poll()`/`read()` failures already throw
  `std::system_error` inside `Stream`; `wait()` returning `false` means
  "timed out," never "something went wrong." `StreamObserver` does not
  catch and repackage those exceptions — they propagate unchanged,
  preserving M10's existing error-handling convention. Command-level
  failure (`status() != DMA_ACCEL_OK`) is orthogonal and checked
  separately via `fence.status()` after `Completed`, matching how
  `CommandGroup` already separates readiness from success/failure.

## Build

Compiles clean (zero warnings), sandbox and target guest VM (`ai-accel`),
alongside the unmodified C0/C1 files.

## Runtime verification

Run on `ai-accel`. Two of the three checks are timing-dependent by
design (that's the actual thing being tested) and both confirmed the
underlying assumption rather than needing to be loosened:

```
== dma-accel C2 StreamObserver smoke test ==
opened /dev/dma_accel0
-- query() immediately after submit() --
query() -> Pending
PASS: query() correctly observed in-flight state
PASS: command completed correctly after wait()
-- wait(fence, 0ms) immediately after submit() --
wait(0ms) -> Pending
PASS: 0ms wait correctly did not block and saw Pending
PASS: command completed correctly within timeout
-- query() after the fence is already known-ready --
PASS: query() on an already-ready fence correctly returned Completed
PASS: StreamObserver verified — non-blocking query() and timeout-bounded wait() both correctly reflect real device state
```

Confirms: `query()` immediately after `submit()` sees `Pending` (Track
A's driver is IRQ-driven — submission returns before the device
necessarily even starts, let alone finishes); `wait(fence, 0ms)` is
deterministically `Pending` for the same reason; a fully-drained fence
correctly reports `Completed` on a subsequent `query()`; and all three
commands used in this test completed with `DMA_ACCEL_OK` and verified
correct COPY data.

## Environment note

Mid-session, `/dev/dma_accel0` was briefly missing on `ai-accel` — driver
module (`dma_accel.ko`) had unloaded (likely a VM restart between
sessions). Re-running `sudo insmod dma_accel.ko` restored it; this is an
environment/session detail, not a Track C code issue. Worth remembering
for future sessions on this VM: check `lsmod | grep dma_accel` before
assuming the device is present.

## Open item carried over

Same as C0/C1: Track A/B remain unsplit in `~/dma-accel-driver/` root.
Decision still deferred.

## Result

C2 marked DONE. Proceed to C3 (Dependency & Synchronization).
