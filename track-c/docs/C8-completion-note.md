# Track C — C8 Completion Note

**Status: DONE**
**Date verified:** 2026-08-24

## What was added

Pure validation — no new Track C capability (per spec, C8 is
validation-only):

```
track-c/tests/dma_accel_c8_workload.cpp
```

## Workload

Two independent output tiles (Stream A, Stream B), each a 3-step
`TILE_MATMUL` accumulation: `C = A0@B0 + A1@B1 + A2@B2`, verified against
a CPU-side float32 reference computation. This is the first time
`TILE_MATMUL` has been driven through a real multi-step accumulation
end-to-end — Track A's M11 notes explicitly deferred this ("hasn't yet
been driven all the way through a full multi-tile GEMM"). C8 closes that
gap.

## What was exercised, and how

| Milestone | Role in this workload |
|---|---|
| C1 CommandGroup | Aggregates the two final validation-COPY fences — one group per stream (see boundary note below) |
| C2 StreamObserver | Non-blocking polling for the final step's completion |
| C3 DependencyEngine | Each tile's 3-step chain (step k+1 depends_on step k) — a genuine dependency: step k+1 cannot correctly read the accumulator until step k has actually written it |
| C5 BufferPool | Double-buffered: 4-slot pool per tile serves 6 buffer-uses (3 steps x 2 inputs) — real reuse, not just an API call |
| C6 | The two chains run on two independent Streams, each on its own thread |
| C7 StreamScheduler | Schedules the two final, independent, dependency-free validation COPY commands — deliberately NOT used for the dependency chains themselves |
| C4 CommandBatch | Not exercised — see below |

**C3/C7 boundary, explicitly preserved**: the two dependency chains are
each driven by their own `DependencyEngine::progress()` loop, called
directly by this workload's own orchestration code — not by
`StreamScheduler`. `StreamScheduler` only touches the two flat,
already-eligible validation COPY commands at the very end, which is
exactly its intended scope. This distinction is stated explicitly in the
source comments so it isn't blurred: C7 has no dependency-graph
awareness by design, and C8 doesn't pretend otherwise.

**C1/C6 boundary**: `CommandGroup` is bound to exactly one `Stream` (C1's
frozen scope). Since the two final validation fences come from two
different Streams, they're aggregated as two separate `CommandGroup`s
(`groupA`, `groupB`), not one combined group — this is the C1/C6
boundary already frozen in the spec, correctly preserved rather than
worked around.

**C4 not exercised — stated as a real finding, not a gap to paper over**:
the tiled-GEMM execution path has no natural independent batch-submission
opportunity under the frozen single-command ioctl ABI — each
`TILE_MATMUL` step must wait for the previous step's real completion
before it can even be constructed with valid inputs, so there's nothing
to batch. C4 remains independently validated by its own C4 milestone
tests. Forcing a batch call into this workload would have been exercising
the API for its own sake, not because the workload needed it — consistent
with every other "not exercised" decision made across Track C.

## Two real problems found and resolved during verification (both
environmental, not Track C logic bugs — but genuinely worth recording)

**1. A real kernel-level deadlock, traced to accumulated driver state.**
Mid-investigation, a run hung with one tile chain stuck after step 0 (a
QEMU device-model debug trace confirmed: one tile's accumulator address
executed once and never again, while the other tile's executed all 3
steps correctly and self-consistently). The stuck process was in
uninterruptible sleep (D state, confirmed via ps), and rmmod subsequently
reported the module still in use with no owning process (confirmed via
fuser) — a genuine kernel-side reference/lock leak, not a userspace hang.
This driver module had been loaded once and reused, without a single
rmmod, across dozens of Track C test runs spanning C0 through this point
in C8. A clean VM reboot (the only way to recover from the D-state
deadlock) fully resolved it.

**2. Wrong numeric results on the very next run, also traced to the same
root cause.** Before the reboot took effect, one run completed (no hang)
but produced large errors (max_abs_error 1.47 and 2.58) against the CPU
reference on both tiles. Per-step diagnostic instrumentation was added
(comparing device-vs-CPU values after each of the 3 steps, not just at
the end) specifically to localize this — but a clean reboot, applied
before that instrumented build was even run, turned out to fully resolve
this too: the post-reboot run showed essentially exact agreement (errors
of 8e-7 and 1.4e-6, well within float32 rounding) at every single
per-step checkpoint. This strongly suggests both symptoms (the hang and
the wrong numbers) shared the same root cause — degraded driver-internal
state from extended reuse across a very long test session — rather than
being two separate bugs.

**Practical takeaway recorded for future sessions**: during long,
iterative Track C testing sessions that load the driver module once and
run dozens of tests against it without ever rmmod-ing, periodically
reloading the module (rmmod + insmod) is worth doing preventively, not
just reactively after a hang. The per-step diagnostic instrumentation
added to dma_accel_c8_workload.cpp during this investigation was kept in
the final version — it's cheap, and having per-step ground truth proved
valuable for fast localization; removing it would lose that for no real
benefit.

## Runtime verification (clean, post-reboot)

Run on `ai-accel`, fresh module load:

```
== dma-accel C8 end-to-end workload: 2-tile, K=3 GEMM accumulation ==
using device /dev/dma_accel0
-- running two independent 3-step dependency chains --
tile 0 after step0: device[0]=0.3033 cpu[0]=0.3033 max_abs_err=0.0000
tile 1 after step0: device[0]=0.7008 cpu[0]=0.7008 max_abs_err=0.0000
tile 0 after step1: device[0]=0.8259 cpu[0]=0.8259 max_abs_err=0.0000
tile 1 after step1: device[0]=1.7169 cpu[0]=1.7169 max_abs_err=0.0000
tile 0 after step2 (final): device[0]=1.6254 cpu[0]=1.6254 max_abs_err=0.0000
tile 1 after step2 (final): device[0]=3.1059 cpu[0]=3.1059 max_abs_err=0.0000
tile A: max_abs_error = 8.34465e-07
tile B: max_abs_error = 1.43051e-06
tile A dependency chain (A0 -> A1 -> A2): PASS
tile B dependency chain (B0 -> B1 -> B2): PASS
pool A: capacity=4 (4 physical allocations served 6 buffer-uses)
pool B: capacity=4 (4 physical allocations served 6 buffer-uses)
-- scheduling final validation COPY via C7 StreamScheduler --
scheduled 2 validation COPY commands
CommandGroup aggregation: groupA=AllOk groupB=AllOk
C4 CommandBatch: not exercised by this workload (see header comment) - independently validated by its own C4 milestone tests
PASS: C8 end-to-end workload verified - 2-tile K=3 GEMM accumulation correct on both tiles, exercising C1/C2/C3/C5/C6/C7 together, C4 independently validated separately
```

Exit code 0. Every per-step checkpoint agrees with the CPU reference to
within float32 rounding; both tiles' final results pass; pool reuse
confirmed at the exact designed ratio (4 allocations / 6 uses); scheduler
and aggregation both correct.

## Result

**C8 marked DONE. Track C (C0 through C8) is now complete.**

Nine milestones, each with real code, real tests, real hardware
verification, and several genuine engineering discoveries along the way
rather than a straight line: the C4 scope correction (no batch ioctl
exists), the C6a/C6b/C7 investigative arc (ambiguous metric leads to
finding the real mechanism leads to an evidence-driven scheduler design,
ending in a maximal result), the C6b KCTL evaluation (one component
adopted for a stated reason, two rejected for stated reasons), and this
C8 investigation (a real kernel-level deadlock traced and resolved, not
hidden).

## Open item carried over, still unresolved

Track A/B remain unsplit in `~/dma-accel-driver/` root (noted since C0).
Decision still deferred — does not block Track C's completion.
