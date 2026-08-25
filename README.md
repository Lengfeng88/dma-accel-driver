# dma-accel-driver

An accelerator software stack built end-to-end against a custom QEMU
PCIe DMA accelerator device model: kernel driver, multi-client resource
management, and a C++ execution runtime — three tracks, each solving a
different systems problem on top of the same hardware.

```
                 DMA Accelerator
                       |
          +------------+------------+
          |            |            |
          v            v            v
       Track A       Track B       Track C
    "Make it       "Make it       "Make execution
     work"          safe/fair"     programmable
                                    and efficient"
          |            |            |
          v            v            v
       Driver       Isolation      Runtime
       DMA/IRQ      Ownership      Commands
       SQ/CQ        Quota          Async
       Compute      Fairness       Dependencies
       Recovery     Multi-client   Batching
                                   Memory
                                   Streams
                                   Scheduling
```

## Status

| Track | Scope | Milestones | Status |
|---|---|---|---|
| [Track A](track-a/) | Hardware/driver bring-up | M0-M11 | Complete |
| [Track B](track-b/) | Multi-client resource management | M10.5-M15 | Complete |
| [Track C](track-c/) | C++ execution runtime | C0-C8 | Complete |

## What each track answers

**Track A - "Can the accelerator be correctly controlled at all?"**
PCIe enumeration, BAR0/MMIO, DMA transfer via IRQ, SQ/CQ command queue,
4-way concurrent hardware execution, reset/recovery, compute opcodes
(`COPY`, `SCALE_ADD`, `TILE_MATMUL`), and a C++ runtime MVP
(`Stream`/`Buffer`/`Fence`) lifting the raw ioctl/mmap/poll/read ABI.
See [`track-a/README.md`](track-a/README.md).

**Track B - "Is it safe and fair when multiple clients share the
device?"** Per-session buffer/completion ownership, a userspace
`Context` identity guard, per-session resource quota, round-robin
scheduling across `Context`s within one process, SQ ring hard-capacity
enforcement, and dynamic fairness quota. Several of these milestones are
real bugs found by auditing existing assumptions, not features built
against a pre-written spec.

**Track C - "Given hardware that already works, how is a complex
workload organized and executed efficiently?"** Nine milestones
(`C0`-`C8`) building a runtime on top of Track A's frozen M10 baseline:
multi-command tracking, non-blocking observation, host-side dependency
scheduling, command batching, buffer pooling, multi-stream concurrency,
round-robin execution scheduling, and a real end-to-end 2-tile GEMM
workload verified against a CPU reference. See
[`track-c/docs/TRACK_C_RUNTIME_SPEC.md`](track-c/docs/TRACK_C_RUNTIME_SPEC.md)
for the full design spec, and `track-c/docs/C*-completion-note.md` for
each milestone's verification record - including the real bugs found
along the way (a scope correction when no batch ioctl turned out to
exist, an investigation into why cross-session hardware overlap doesn't
happen automatically, and a genuine kernel-level deadlock traced to
accumulated driver state during a long test session).

## Repository structure

```
dma-accel-driver/
├── track-a/           Kernel driver + C++ runtime MVP (Makefile here)
├── track-b/           Context/Scheduler + verification tests
├── track-c/
│   ├── runtime/       C1-C7 runtime components
│   ├── tests/         Smoke tests, benchmarks, the C8 workload
│   ├── third_party/   Vendored kctl (systems/concurrency library)
│   └── docs/          Frozen spec + per-milestone completion notes
```

Note on `track-a/dma_accel_drv.c`: this single kernel driver file also
contains Track B's kernel-side milestones (M10.5/M12/M14/M15) - those
were implemented as modifications to this file, not separate ones, since
session/queue state lives in one module. See
[`track-a/README.md`](track-a/README.md) for the full explanation.

## Build

**Kernel driver (Track A, including Track B's kernel-side changes):**

```bash
cd track-a
make
sudo insmod dma_accel.ko
```

**Userspace (Track A/B/C):** currently compiled directly with `g++`, no
unified build system yet. Each `track-c/docs/C*-completion-note.md`
includes the exact compile/link commands used to build and verify that
milestone. Example, for Track C's C8 end-to-end workload:

```bash
cd track-c
g++ -std=c++17 -Wall -Wextra -pthread -c runtime/dma_accel_runtime.cpp -I runtime -o /tmp/runtime.o
g++ -std=c++17 -Wall -Wextra -pthread -c runtime/dma_accel_command_group.cpp -I runtime -o /tmp/command_group.o
g++ -std=c++17 -Wall -Wextra -pthread -c runtime/dma_accel_dependency.cpp -I runtime -o /tmp/dependency.o
g++ -std=c++17 -Wall -Wextra -pthread -c runtime/dma_accel_buffer_pool.cpp -I runtime -o /tmp/buffer_pool.o
g++ -std=c++17 -Wall -Wextra -pthread -c runtime/dma_accel_stream_observer.cpp -I runtime -o /tmp/observer.o
g++ -std=c++17 -Wall -Wextra -pthread -c runtime/dma_accel_stream_scheduler.cpp -I runtime -o /tmp/scheduler.o
g++ -std=c++17 -Wall -Wextra -pthread -c tests/dma_accel_c8_workload.cpp -I runtime -o /tmp/c8_workload.o
g++ -std=c++17 -Wall -Wextra -pthread -o dma_accel_c8_workload \
  /tmp/c8_workload.o /tmp/command_group.o /tmp/dependency.o /tmp/buffer_pool.o \
  /tmp/observer.o /tmp/scheduler.o /tmp/runtime.o
sudo ./dma_accel_c8_workload /dev/dma_accel0
```

## Git history

Every milestone is tagged, so any point in the project's development can
be checked out directly:

```
a-m11    Track A complete (M0-M11)
b-m15    Track B complete (M10.5-M15)
c0..c8   Track C, one tag per milestone
```

```bash
git checkout c3    # e.g. Track C's DependencyEngine right after it landed
git log --oneline  # full milestone-by-milestone history
```

This history was committed as verified working snapshots after each
track/milestone was developed and tested - not reconstructed to look
incrementally authored after the fact.

## kctl

`track-c/third_party/kctl/` is a vendored systems/concurrency library
(RAII guards, lock-free SPSC/MPMC ring buffers, sharded thread pools,
polymorphic task queue) used selectively - not on every milestone. Track
C's design principle: *KCTL supplies concurrency infrastructure only
when the runtime's own semantics genuinely require it.* It first earned
a real place at Track C's C6b milestone (multi-threaded stream
execution), where `kctl::MpmcBoundedQueue` was adopted for a stated
reason, while `kctl::ThreadPoolMpmc`/`ThreadPoolSharded` and
`kctl::DeviceHandle` were evaluated and explicitly rejected for that
milestone, also with stated reasons - see
[`track-c/docs/C6b-completion-note.md`](track-c/docs/C6b-completion-note.md).
