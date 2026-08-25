# Track C — C6a Completion Note

**Status: DONE**
**Date verified:** 2026-08-20/21

## What was added

Pure verification — no new Track C production code (see the C6 design
discussion: C6a's whole point is that M10's `Stream` already supports
multiple independent sessions with zero changes needed):

```
track-c/tests/dma_accel_multistream_overlap_test.cpp          (superseded methodology, kept for record)
track-c/tests/dma_accel_multistream_overlap_copy_test.cpp     (superseded methodology, kept for record)
track-c/tests/dma_accel_multistream_timeline_test.cpp         (block submission — the real finding)
track-c/tests/dma_accel_multistream_timeline_interleaved_test.cpp (interleaved submission — confirms mechanism)
```

## The investigation, in order (this is the actual value of C6a)

**Attempt 1 — total duration ratio (T_concurrent / T_single).** Measured
~1.8–1.99 across both `TILE_MATMUL` and `OPCODE_COPY` variants —
consistently near 2.0, suggesting serialization. But this method turned
out to be fundamentally ambiguous: a shared, fixed hardware concurrency
budget (`MAX_INFLIGHT` commands device-wide, not per-session) produces a
~2x ratio for 2x total commands *regardless* of whether the two
sessions' commands are genuinely interleaved or fully serialized — total
duration alone cannot distinguish those cases. This was caught before
drawing a conclusion from it, and the method was replaced rather than
patched.

**Attempt 2 — completion timeline, block submission** (all of A's 8
commands submitted, then all of B's 8, no waiting in between). Real
result on `ai-accel`:

- All 16 commands admitted into the SQ ring within 383µs — B's commands
  were never blocked waiting to submit.
- Completions arrived in exactly four 4-command waves, ~50ms apart
  (precisely matching `MAX_INFLIGHT=4` and Track A's M7 self-test
  timing).
- But **all of A's two waves completed before any of B's began** — 1
  transition, strictly serial at the completion level.

This combination of facts (B fully admitted early, but not dispatched
until A fully drained) pointed to a specific, falsifiable mechanism:
**the device dispatches strictly in SQ-ring arrival order, 4 at a time,
with no session-awareness at the dispatch level.** Since block submission
put A's 8 commands in ring positions 0–7 and B's in 8–15, the device
simply worked through 0–7 (two waves of A) before reaching 8–15 (two
waves of B).

**Attempt 3 — completion timeline, interleaved submission** (A0, B0, A1,
B1, ... at the ioctl level, so ring positions alternate between
sessions). This directly tests the hypothesis from attempt 2. Real
result:

```
A A B B | A A B B | A A B B | A A B B     (7 transitions)
```

Confirmed: genuinely interleaved completions, 7 transitions across 16
completions — matching the predicted FIFO-by-ring-position mechanism
exactly.

## Conclusion (this is the real C6a finding, more valuable than a simple
pass/fail)

Cross-session hardware overlap **is real and achievable**, but it is a
**submission-order property, not an automatic scheduling behavior**.
Nothing in the driver or device interleaves dispatch across sessions —
Track B's fairness/quota mechanism (M12/M15) governs *admission* (whether
a command can enter the ring at all), not *dispatch order* once admitted.
If a caller submits one session's commands as an unbroken block, they
will be dispatched as an unbroken block. If a caller interleaves
submission across sessions, dispatch interleaves too.

**This directly defines what C7 (Scheduling) needs to do**: achieving
real cross-session overlap in practice requires a P2-level scheduler that
actively interleaves submission across streams — this is not something
lower layers (driver, device) provide for free. C7's job is now backed by
concrete evidence of why it's needed, not a speculative architecture
diagram.

## C6a acceptance criteria (met)

Per the C6 design discussion, C6a does not require a specific timing
ratio or perfect alternation — it requires that neither session is left
waiting for the other to fully drain before making any progress, which
the interleaved-submission run demonstrates unambiguously (7 transitions,
not 1).

## Test harness notes (environment, not library issues)

- VM restarts between sessions on `ai-accel` clear both `/dev/dma_accel0`
  (driver module unloaded) and `/tmp` (compiled `.o` files) — became a
  recurring, expected step: check `lsmod | grep dma_accel` and recompile
  dependencies before assuming either is still present.
- The `baseline_stream` in the original overlap-ratio tests needed to be
  scoped to close before the concurrent phase began, to avoid Track B's
  per-session quota counting it as a third active session — a real
  lifecycle discipline issue, caught and fixed, though ultimately moot
  once the ratio methodology itself was replaced.

## KCTL note

Still no `kctl` involvement — C6a is single-threaded start to finish
(the timeline tests poll both Streams from one thread; no OS-level
concurrency exists yet). Per §6.5, that's still correctly deferred to
C6b, which is next.

## Open item carried over

Same as C0–C5: Track A/B remain unsplit in `~/dma-accel-driver/` root.
Decision still deferred.

## Result

C6a marked DONE, with a well-evidenced mechanism, not just a pass/fail.
Proceed to C6b (Concurrent runtime execution — genuine host-side
multithreading, and the first real KCTL evaluation point per §6.5).
