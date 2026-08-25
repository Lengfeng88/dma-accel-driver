# Track C — C1 Completion Note

**Status: DONE**
**Date verified:** 2026-08-20

## What was added

Purely additive to C0 — no C0 file was modified:

```
track-c/runtime/dma_accel_command_group.hpp
track-c/runtime/dma_accel_command_group.cpp
track-c/tests/dma_accel_command_group_smoke_test.cpp
```

## Design recap

`CommandGroup`: tracks a set of commands submitted on **one** Stream as a
single logical execution unit. Explicitly not a batching/submission-
optimization primitive (that's C4's `CommandBatch` — a deliberately
separate type). Explicitly not cross-Stream (that's C6).

Lifecycle: `OPEN → SEALED`, with aggregate result (`Pending` / `AllOk` /
`SomeFailed`) computed on demand by `query()`/`wait()`, not a separate
lifecycle state — consistent with M10's rule that there's no background
thread pushing completions; everything is pulled via explicit
`Stream::pump()`.

`add()` after seal, and `query()`/`wait()` before seal, both throw
`std::logic_error` — matches the "wrong-time call is a caller bug"
convention `Fence::status()` already established in M10.

`wait()` implementation detail worth keeping in mind for later
milestones: it repeatedly calls `Stream::wait()` on whichever tracked
fence isn't ready yet, relying on the fact that all fences in the group
share the same Stream's completion queue — so pumping for any one of them
advances all of them. No new blocking primitive was added to `Stream`;
C1 composes entirely from what M10 already exposed.

## Build

Compiles clean (zero warnings) both in isolation (sandbox) and on the
target guest VM (`ai-accel`), alongside the unmodified C0
`dma_accel_runtime.cpp`.

## Runtime verification

Run on `ai-accel`, same driver/device state as C0:

```
== dma-accel C1 CommandGroup smoke test ==
opened /dev/dma_accel0
-- happy path: 4 commands in one CommandGroup --
added 4 commands to group
PASS: all 4 commands completed AllOk, all data verified correct
-- lifecycle guards --
PASS: query() before seal() threw std::logic_error
PASS: add() after seal() threw std::logic_error
PASS: empty sealed group is vacuously AllOk
PASS: CommandGroup verified — aggregation, sealing, and lifecycle guards all correct
```

All four checks passed: happy-path aggregation and data correctness
across 4 independently submitted COPY commands, both lifecycle guards
(`add()` post-seal, `query()`/`wait()` pre-seal) throwing as designed,
and the empty-sealed-group vacuous-truth case.

## Open item carried over

Same as noted in the C0 completion note: Track A/B remain unsplit in
`~/dma-accel-driver/` root. Decision deferred (per spec §2 note): only
Track C requires physical isolation, since it's the actively evolving
tree.

## Result

C1 marked DONE. Proceed to C2 (Observable Async Execution).
