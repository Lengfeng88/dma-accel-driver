# Track C — C++ Accelerator Execution Runtime (C0-C8)

"Given hardware that already works, how is a complex workload organized
and executed efficiently?"

Nine milestones, each with real code, a real test, real hardware
verification, and a written completion note — several of them containing
genuine engineering discoveries (a scope correction, a multi-run
investigation into hardware overlap, a real kernel deadlock traced and
resolved) rather than a straight line from spec to done. See the
top-level [`README.md`](../README.md) for how this fits into the
three-track architecture, and
[`docs/TRACK_C_RUNTIME_SPEC.md`](docs/TRACK_C_RUNTIME_SPEC.md) for the
full frozen design spec this track was built against.

## Milestones

| Tag | Milestone | What it is |
|---|---|---|
| `c0` | C0 Runtime MVP Baseline | Frozen snapshot import of Track A M10 (`Buffer`/`Stream`/`Fence`) - no new code |
| `c1` | C1 CommandGroup | Tracks a set of commands submitted on one Stream as one logical unit |
| `c2` | C2 StreamObserver | Non-blocking `query()` + timeout-bounded `wait()`, without modifying `Fence`/`Stream` |
| `c3` | C3 DependencyEngine | Host-side dependency DAG (`PendingCommand`), no device-side dependency ABI |
| `c4` | C4 CommandBatch | Ordered build/freeze/dispatch - explicitly does NOT reduce syscall count (no batch ioctl exists) |
| `c5` | C5 BufferPool | Fixed-size buffer reuse via RAII, relieving real per-session quota pressure hit during C3 testing |
| `c6` | C6a/C6b Multi-stream | Hardware overlap investigation + first real KCTL integration |
| `c7` | C7 StreamScheduler | Single-threaded round-robin scheduling, 15/16 max transitions achieved |
| `c8` | C8 End-to-End Workload | Real 2-tile K=3 GEMM, verified against a CPU reference |

Each tag is checkable directly (`git checkout c3`), and each milestone
has a completion note in [`docs/`](docs/) with the actual verification
output from the target hardware/VM, not just a pass/fail summary.

## Structure

```
track-c/
├── runtime/       CommandGroup, StreamObserver, DependencyEngine,
│                  CommandBatch, BufferPool, StreamScheduler (C1-C7)
├── tests/         Smoke tests, the C6a/C6b overlap investigation,
│                  and the C8 end-to-end workload
├── third_party/
│   └── kctl/      Vendored systems/concurrency library (see below)
└── docs/
    ├── TRACK_C_RUNTIME_SPEC.md      Frozen design spec, source of truth
    └── C0..C8-completion-note.md    Per-milestone verification record
```

## Build

No unified build script yet - each file is compiled directly with
`g++`. Example (C1's CommandGroup, against the frozen C0 baseline):

```bash
cd track-c
g++ -std=c++17 -Wall -Wextra -c runtime/dma_accel_runtime.cpp -I runtime -o /tmp/runtime.o
g++ -std=c++17 -Wall -Wextra -c runtime/dma_accel_command_group.cpp -I runtime -o /tmp/command_group.o
g++ -std=c++17 -Wall -Wextra -c tests/dma_accel_command_group_smoke_test.cpp -I runtime -o /tmp/group_smoke.o
g++ -std=c++17 -Wall -Wextra -o dma_accel_command_group_smoke_test /tmp/group_smoke.o /tmp/command_group.o /tmp/runtime.o
sudo ./dma_accel_command_group_smoke_test /dev/dma_accel0
```

Each `docs/C*-completion-note.md` has the exact commands used to build
and run that milestone's tests, including any dependencies on earlier
milestones' `.o` files.

## kctl

`third_party/kctl/` is used selectively, not on every milestone. Track
C's rule (frozen in `docs/TRACK_C_RUNTIME_SPEC.md` section 6.5): C0-C5
stayed single-threaded by design (inherited from M10's own invariant),
so KCTL's concurrency primitives had no genuine use until C6, the first
milestone with real concurrency. At C6b, `kctl::MpmcBoundedQueue` was
adopted for lock-free multi-producer completion reporting across real OS
threads; `kctl::ThreadPoolMpmc`/`ThreadPoolSharded` and
`kctl::DeviceHandle` were evaluated and explicitly rejected, each with a
stated reason - see `docs/C6b-completion-note.md`.
