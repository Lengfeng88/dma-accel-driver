# Track A — Hardware/Driver Bring-up (M0–M11)

"Make the accelerator work." PCIe enumeration, BAR0/MMIO, DMA+IRQ,
SQ/CQ command queue, compute opcodes, recovery. See the top-level
`TRACK_C_RUNTIME_SPEC.md` for how this fits into the three-track
architecture (Track A / Track B / Track C).

## Note on `dma_accel_drv.c`

This file is physically located here because driver bring-up (M0–M8) and
the compute-opcode work (M11) are Track A deliverables. However, **this
file also contains Track B's kernel-side milestones** (M10.5 session
ownership, M12 resource quota, M14 SQ ring isolation, M15 dynamic
fairness) — those were implemented as modifications to this same driver
file, not as separate files, since kernel-side session/queue state lives
in one module.

This is stated explicitly so the `track-a/` / `track-b/` directory split
isn't read as implying a cleaner separation than actually exists at the
kernel level. The split is real and meaningful for the *userspace*
components (`dma_accel_runtime.*` is pure Track A; `dma_accel_context.*`
and `dma_accel_scheduler.*` are pure Track B) — it does not apply to this
one kernel driver file, which both tracks' kernel-side work shares.

See `../track-b/` for Track B's milestone docs (`docs/M10.5-...` through
`docs/M15-fairness.md`), which describe the specific changes made to this
file at each milestone.

## Build
make # builds dma_accel.ko
make clean
