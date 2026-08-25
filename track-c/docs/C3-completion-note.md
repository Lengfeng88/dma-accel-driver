# Track C — C3 Completion Note

**Status: DONE**
**Date verified:** 2026-08-20

## What was added

Purely additive to C0/C1/C2 — no existing file modified:

```
track-c/runtime/dma_accel_dependency.hpp
track-c/runtime/dma_accel_dependency.cpp
track-c/tests/dma_accel_dependency_smoke_test.cpp
```

## Design recap

`PendingCommand` + `DependencyEngine`: host-side, runtime-managed
dependency tracking. A command isn't submitted to the driver until its
declared dependencies are satisfied. No device-side dependency ABI exists
or was added — `dma_accel_regs.h`'s SQ descriptor has no field for "wait
on cmd_id X"; adding one would mean extending Track A's register spec and
QEMU device model, out of scope for Track C entirely.

Three genuinely different lifetime rules for `depends_on()`, each forced
by a real constraint rather than chosen for symmetry:

- **`depends_on(Fence&&)`** — ownership transfer. Checked against the
  actual M10 `Fence` implementation: its `shared_ptr<FenceState>` is
  private with no accessor, so sharing ownership without modifying Track
  A isn't possible. Moving the `Fence` in instead makes dangling
  structurally impossible — the caller's variable is empty afterward, so
  there's no other reference left to dangle. Mirrors
  `CommandGroup::add(Fence&&)`'s precedent from C1.
- **`depends_on(const CommandGroup&)`** — non-owning pointer, same
  convention as `Stream*` inside `CommandGroup`/`StreamObserver`.
- **`depends_on(const PendingCommand&)`** — safe `shared_ptr` copy of the
  other command's internal state. No caller lifetime obligation at all,
  since `DependencyEngine` keeps every registered command's state alive
  for the engine's own lifetime.

DAGs are supported via `PendingCommand`-on-`PendingCommand` edges; cycles
are rejected at the `depends_on()` call that would create one (a
reachability check from the new dependency back to `this`, including the
trivial self-dependency case), not discovered later as commands stuck
silently Pending forever.

Failure propagates: if a dependency completes with a non-OK status, the
dependent is marked `blocked` (permanent, checked before ever attempting
submission) rather than left Pending with no diagnostic.

No scheduling policy: when a `progress()` call finds more than one
newly-eligible command, they submit in FIFO registry order — explicitly
the null case, not a C7 decision. C3 answers "is this eligible yet?"; C7
(later) answers "which eligible work goes first under limited hardware
capacity?"

## Two real bugs found during verification (both fixed, in the test
harness — not in DependencyEngine itself)

**1. Busy-spin polling raced against real device latency.** The original
test drove `progress()` in a tight loop with no delay between calls.
`progress()` is deliberately non-blocking (one `pump()` per call, by
design). 1000 rapid non-blocking iterations complete in microseconds —
far faster than the tens-of-milliseconds real completion latency observed
in Track A's M7 concurrency test dmesg output. The loop exhausted its
budget before the hardware had a realistic chance to finish anything, so
dependencies that were actually correct still read as never-satisfied.
Fixed by adding a 5ms sleep between non-blocking `progress()` calls in
the test harness (not in `DependencyEngine` itself — a real caller
integrating this into an event loop would drive `progress()` from
whatever already wakes it periodically, not busy-spin).

**2. A dangling non-owning pointer caused a real segfault, exposing a
genuine test-design flaw.** Because scenario 1 and 2 initially failed
(due to bug #1) and returned early, their local `Buffer`/`CommandGroup`
objects were destroyed — but the single `DependencyEngine` shared across
all five scenarios still held non-owning references to them
(`group_deps` pointers, captured `Buffer*` in submission closures) in its
registry from the still-pending entries those scenarios left behind.
Scenario 4's later `progress()` call iterated the *entire* shared
registry, including those stale entries, and dereferenced a pointer to an
already-destroyed `CommandGroup` — use-after-free, segfault. This is
exactly the lifetime contract documented in the design (non-owning
references must outlive their resolution) — the test violated it, the
library didn't. Fixed by giving each scenario its own `Stream` (its own
driver session) and its own `DependencyEngine`, isolating failures
between scenarios entirely.

**3. Session buffer quota, not a bug but worth recording.** After fix
#1/#2 were applied and scenarios 1–4 started passing, scenario 5 hit
`ioctl(DMA_ACCEL_IOC_BUFFER_ALLOC): Disk quota exceeded`. Root cause:
Track B's M12 caps buffers at 16 per session (64 device slots / 4-way
concurrency). M10's `Buffer` has no `BUFFER_FREE` ioctl (deliberately
deferred per Track A's spec), so a single long-lived `Stream` shared
across five buffer-heavy scenarios (3+5+4+4+3 = 19 buffers) ran straight
into that real, correctly-enforced quota. This wasn't a Track C bug and
wasn't a Track B bug either — both worked exactly as designed; the test's
buffer usage pattern was the mismatch. Resolved by the same per-scenario
`Stream` split as fix #2 (each session now well under 16 buffers).

## Build

Compiles clean (zero warnings) on the target guest VM (`ai-accel`),
alongside the unmodified C0/C1/C2 files. Note: the smoke test links with
`-pthread` (needed for `std::this_thread::sleep_for`, test-harness only —
`DependencyEngine` itself has no threading dependency).

## Runtime verification

Run on `ai-accel`, all five scenarios:

```
== dma-accel C3 DependencyEngine smoke test ==
using device /dev/dma_accel0
-- 1. single Fence dependency --
PASS: B correctly waited for A before submitting, data propagated correctly
-- 2. CommandGroup dependency --
PASS: C correctly waited for the entire group before submitting
-- 3. three-stage PendingCommand chain --
PASS: 3-stage chain resolved in dependency order, data correct end-to-end
-- 4. failure propagation (blocked dependent) --
confirmed bad-length command completed with DMA_ACCEL_ERR_LENGTH as expected
PASS: dependent correctly blocked and stays blocked, never submitted
-- 5. cycle rejection --
PASS: two-node cycle correctly rejected
PASS: self-dependency correctly rejected
PASS: DependencyEngine verified — Fence/Group/chain dependencies, failure propagation, and cycle rejection all correct
```

## Open item carried over

Same as C0/C1/C2: Track A/B remain unsplit in `~/dma-accel-driver/` root.
Decision still deferred.

## Result

C3 marked DONE. Proceed to C4 (Command Batching).
