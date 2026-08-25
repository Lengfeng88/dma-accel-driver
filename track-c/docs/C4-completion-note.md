# Track C — C4 Completion Note

**Status: DONE**
**Date verified:** 2026-08-20

## What was added

Purely additive to C0/C1/C2/C3 — no existing file modified:

```
track-c/runtime/dma_accel_command_batch.hpp
track-c/runtime/dma_accel_command_batch.cpp
track-c/tests/dma_accel_command_batch_smoke_test.cpp
```

## Scope correction found before implementation (not a bug — a real ABI wall)

C4's original milestone description (submission overhead reduction /
reduced syscall count) turned out to be undeliverable as written. Checked
against the actual driver ABI in `dma_accel_regs.h`:

```c
#define DMA_ACCEL_IOC_SUBMIT _IOWR(DMA_ACCEL_IOC_MAGIC, 2, struct dma_accel_submit)
```

Single-command ioctl, no count field, no descriptor array — there is no
batch ioctl. True syscall-count reduction would require extending Track
A's register spec, driver, and QEMU device model, which is out of scope
for Track C entirely (same category of wall C3 hit with device-side
dependency ABI).

**C4 was explicitly redefined** rather than either faking the capability
or silently dropping the milestone: from "Command Batching (submission
overhead reduction)" to "Command Construction & Batching (ordered
build/freeze/dispatch of multiple commands, with no claim of kernel-level
batching)." `CommandBatch::submit()` internally still calls
`Stream::submit()`/etc. once per command — exactly the same ioctl cost as
a caller-written loop. The value C4 actually provides: a build → freeze →
dispatch lifecycle, fixed submission order once frozen, and automatic
production of a sealed `CommandGroup` over the results — convenience and
structure, not performance. If a real, measured workload later shows
per-ioctl overhead is an actual bottleneck, that evidence is what
justifies proposing a batch ioctl as a Track A extension request (§7) —
not something to speculatively build now.

## Design recap

- `CommandBatch`: `OPEN → FROZEN → SUBMITTED` lifecycle. `append()` after
  freeze, or `submit()` before freeze or a second time, all throw
  `std::logic_error`. `freeze()` is idempotent, matching
  `CommandGroup::seal()`'s C1 precedent.
- Explicitly does not implement dependency logic — if a batched command
  needs a dependency, it doesn't belong in a `CommandBatch`; use C3's
  `DependencyEngine` instead. Deliberately not merged, same reasoning as
  why `CommandGroup` and dependency tracking were kept separate.
- **Known, documented, unrecovered gap**: if a command mid-batch throws
  `std::system_error` during `submit()` (e.g. an out-of-bounds
  offset+len), commands submitted before the failure have already
  reached hardware, but their `Fence`s are lost — `submit()`'s local
  `CommandGroup` is destroyed as the exception unwinds, and there is no
  channel to return a partial result. This is stated plainly in the
  header rather than papered over; building real partial-failure
  recovery was judged premature engineering with no workload demanding
  it yet. `submitted_` is set before the dispatch loop specifically so a
  second `submit()` call after such a failure refuses to retry rather
  than risking double-submission of whatever already went through.

## Build

Compiles clean (zero warnings) on the target guest VM (`ai-accel`),
alongside the unmodified C0/C1/C2/C3 files.

## Runtime verification

Run on `ai-accel`:

```
== dma-accel C4 CommandBatch smoke test ==
using device /dev/dma_accel0 (each scenario opens its own Stream/session)
-- 1. happy path: build -> freeze -> submit --
appended 4 commands, frozen=0
PASS: all 4 batched commands completed AllOk, data verified correct
-- 2/3/4. lifecycle guards --
PASS: append() after freeze() threw std::logic_error
PASS: submit() before freeze() threw std::logic_error
PASS: second submit() call threw std::logic_error (no double-submission)
-- 5. mid-batch ioctl failure (documented, unrecovered gap) --
confirmed submit() threw on the invalid command, as documented
PASS: mid-batch failure behaved exactly as documented — submit() threw, no retry/double-submission possible, and command 0's Fence is (by design, not by bug) unrecoverable through this API
PASS: CommandBatch verified — build/freeze/submit, lifecycle guards, and the documented mid-batch-failure behavior all correct
```

Confirms: 4-command batch completes correctly with verified data; all
three lifecycle guards throw as designed; and the documented mid-batch
failure gap behaves exactly as specified — a real, verified limitation,
not an assumed one.

## Open item carried over

Same as C0–C3: Track A/B remain unsplit in `~/dma-accel-driver/` root.
Decision still deferred.

## Result

C4 marked DONE. Proceed to C5 (Runtime Memory Management).
