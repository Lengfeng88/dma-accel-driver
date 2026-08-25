# Track C — C0 Completion Note

**Status: DONE**
**Date verified:** 2026-08-20

## What was imported

Frozen snapshot of Track A M10 (Runtime MVP), copied verbatim into
`track-c/`:

```
track-c/runtime/dma_accel_runtime.hpp
track-c/runtime/dma_accel_runtime.cpp
track-c/runtime/dma_accel_regs.h        <- corrected: C0's real file
                                            boundary is 4 files, not 3;
                                            this driver-ABI header was
                                            missing from the initial pass
track-c/tests/dma_accel_runtime_smoke_test.cpp
```

## Diff check

All four files confirmed byte-for-byte identical to the Track A M10
source (`diff` clean against originals at import time).

## Build

```
g++ -std=c++17 -Wall -Wextra -c runtime/dma_accel_runtime.cpp -I runtime -o runtime.o
g++ -std=c++17 -Wall -Wextra -c tests/dma_accel_runtime_smoke_test.cpp -I runtime -o smoke.o
g++ -std=c++17 -Wall -Wextra -o dma_accel_runtime_smoke_test smoke.o runtime.o
```

Zero warnings, on both the sandbox verification pass and the target
Ubuntu Noble guest VM (`ai-accel`) pass.

## Runtime verification

Run on `ai-accel` guest VM, driver `dma_accel.ko` loaded, device
`0000:00:04.0` (PCI ID `1234:da00`) present, `/dev/dma_accel0` node
present:

```
== dma-accel M10 runtime smoke test ==
opened /dev/dma_accel0
allocated buffer_id=0 and buffer_id=1 (4096 bytes each)
submitted, kernel-assigned cmd_id=0x2
completion: status=0
PASS: Stream/Buffer/Fence API verified, 4096 bytes copied correctly
```

Matches expected M10 behavior: Stream/Buffer/Fence lift of the M9 raw
ioctl/mmap/poll/read ABI is a correct lift, not just a plausible-looking
one — confirmed independently, outside Track A's original project
context.

## Open item raised during this verification (not yet resolved)

`~/dma-accel-driver/` (the guest VM project root) does **not** currently
have Track A and Track B separated into `track-a/`/`track-b/`
subdirectories as pictured in `TRACK_C_RUNTIME_SPEC.md` §2. All Track A
and Track B source files (driver, runtime, `dma_accel_context.*`,
`dma_accel_scheduler.*`, and their respective tests) live together in the
project root. Only `track-c/` exists as an isolated subdirectory so far,
because it was created fresh for this milestone.

This does not affect C0's validity — the frozen-snapshot files Track C
uses are still verified byte-identical to their root-level originals, and
nothing in the root was modified. But the `track-a/` / `track-b/` split
in the spec's directory diagram is aspirational, not yet real on disk.
Decide separately whether to retroactively split Track A/B into their
own directories, or treat the spec's diagram as describing three
*logical* trees (only Track C's needs to be physically isolated, since
it's the new one) rather than three fully mirrored physical ones.

## Result

C0 marked DONE. Track C's baseline is confirmed equivalent to Track A
M10. Proceed to C1 (Multi-command Submission) design.
