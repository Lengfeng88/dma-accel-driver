# Track C — C7 Completion Note

**Status: DONE**
**Date verified:** 2026-08-21

## What was added

Purely additive to C0-C6b — no existing file modified:

```
track-c/runtime/dma_accel_stream_scheduler.hpp
track-c/runtime/dma_accel_stream_scheduler.cpp
track-c/tests/dma_accel_stream_scheduler_test.cpp
```

## Design recap — directly evidence-driven, not speculative

C6a (single thread, manually alternating submission) achieved 7
cross-stream completion transitions out of 16 commands. C6b (two real OS
threads, uncoordinated) achieved only 5 — deliberate single-threaded
interleaving outperformed naive real concurrency. C7 therefore does
**not** introduce more threading; it generalizes C6a's manually-written
technique into a reusable, single-threaded `StreamScheduler`. No new
KCTL usage — this milestone's mechanism is submission *order*, not
concurrent execution, and the concurrency-shaped evaluation already
happened at C6b.

`StreamScheduler`: registers N `Stream`s (non-owning, same lifetime
convention as `CommandGroup`/`DependencyEngine`), accepts queued
`std::function<Fence()>` closures per stream (same convention as C3's
`PendingCommandState` and C4's `CommandBatch`), and `run()` performs
exact round-robin — one submission per registered stream per pass, in
registration order, looping until every queue is empty. Returns every
resulting `Fence`, tagged by stream index, in actual submission order.

**Scope boundary, stated explicitly**: C7 v1 does not integrate with
C3's `DependencyEngine` — it schedules flat, already-eligible closures,
with no dependency-graph awareness. C3 answers "is this eligible?"; C7
answers "given several eligible commands across streams, what order
should they be presented?" Combining the two is a real, larger
capability with no demonstrated need yet — not built speculatively.

No priority, no fairness weighting, no client identity — pure
round-robin is the explicit "no policy" null case, consistent with the
C3/C7 boundary already frozen in spec section 5.

## Runtime verification

Run on `ai-accel`, 2 streams x 8 commands each, **enqueued in block order**
(all of stream A, then all of stream B) — deliberately, to prove the
interleaving credit belongs to the scheduler, not caller discipline:

```
== dma-accel C7 StreamScheduler verification ==
using device /dev/dma_accel0, 2 streams, 8 commands each, round-robin scheduling
submission-order transitions (scheduler's own interleaving): 15 (expect close to 15 for exact round-robin over 2 streams x 8 each)
-- completion timeline --
     51325 us  stream=0 cmd=0x12
     51336 us  stream=1 cmd=0x13
     51341 us  stream=0 cmd=0x14
     51344 us  stream=1 cmd=0x15
    100674 us  stream=0 cmd=0x16
    100683 us  stream=1 cmd=0x17
    100685 us  stream=0 cmd=0x18
    100688 us  stream=1 cmd=0x19
    151074 us  stream=0 cmd=0x1a
    151081 us  stream=1 cmd=0x1b
    151083 us  stream=0 cmd=0x1c
    151085 us  stream=1 cmd=0x1d
    201104 us  stream=0 cmd=0x1e
    201108 us  stream=1 cmd=0x1f
    201110 us  stream=0 cmd=0x20
    201112 us  stream=1 cmd=0x21
-- summary --
total completions: 16, completion-order transitions: 15
PASS: 15 completion-order transitions, at or above C6a's manual baseline (7) — StreamScheduler successfully generalizes C6a's proven technique into a reusable component, confirmed with commands enqueued in block order (all of A, then all of B) — the interleaving credit belongs entirely to the scheduler, not caller discipline
```

**15 out of a maximum possible 15 transitions** (16 completions, every
single one alternating stream from the previous) — perfect interleaving,
both at submission order (deterministic, guaranteed by round-robin logic)
and at actual completion order (confirming the interleaving survives all
the way through real device execution, not just the submission call).
Exceeds both C6a (7) and C6b (5) by a wide margin.

## Progression summary across C6a -> C6b -> C7

| Milestone | Method | Transitions (of 16) |
|---|---|---|
| C6a | Manual alternation, 1 thread | 7 |
| C6b | Uncoordinated real threads | 5 |
| C7 | StreamScheduler round-robin, 1 thread | 15 (max) |

This is a clean, evidence-backed engineering story: measured a naive
approach, found a confound, fixed the measurement, discovered the real
mechanism, tested a stronger real-concurrency alternative, found it
underperformed the simpler approach, and built the simpler approach into
a correct, reusable, maximally-effective component.

## KCTL note

Consistent with section 6.5: C7 introduces no new KCTL usage. The
concurrency-shaped evaluation happened once, at C6b, with a stated
outcome (one component used, two rejected). C7's mechanism is
deterministic single-threaded ordering, not concurrency — no
re-evaluation needed.

## Open item carried over

Same as C0-C6b: Track A/B remain unsplit in `~/dma-accel-driver/` root.
Decision still deferred.

## Result

C7 marked DONE, with a clean, maximal result exceeding both C6a and C6b.
Proceed to C8 (End-to-End Workload) — validation only, no new capability,
exercising C1-C7 together against a real multi-tile GEMM workload.
