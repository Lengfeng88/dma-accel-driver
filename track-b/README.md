# Track B — Multi-Client Resource Management (M10.5-M15)

"Is it safe and fair when multiple clients share the device?"

Started from a correctness review of Track A's M10 session semantics,
not a pre-planned feature list - several of these milestones (M14
especially) are real bugs found by auditing existing assumptions, not
features implemented against a spec. See the top-level
[`README.md`](../README.md) for how this fits into the three-track
architecture (Track A / Track B / Track C).

## Milestones

| Milestone | What it closes | Layer |
|---|---|---|
| M10.5 | Stream=fd=unique ownership wasn't kernel-enforced - any fd could touch/free any other fd's buffers, and one session's `read()` could silently steal another's completion. Both closed: per-session buffer ownership and completion routing. | Kernel (`../track-a/dma_accel_drv.c`) |
| M11 | `Stream` has no client identity, so passing one client's `Buffer` into another's `submit()` only surfaced as an opaque kernel `EPERM`. `Context` adds a name and catches it as `std::logic_error` in userspace, before any syscall. | Userspace (this dir) |
| M12 | `dev->buffers[]` had no per-session cap - one greedy client could allocate all 64 slots and starve everyone else. Quota grounded in the device's real 4-way concurrency ceiling (64/4=16). | Kernel (`../track-a/dma_accel_drv.c`) |
| M13 | A process managing several `Context`s had to manually interleave its own `submit()` calls for fairness. `Scheduler` does strict round-robin dispatch instead - explicitly scoped to one process. | Userspace (this dir) |
| M14 | Real bug: the SQ ring's physical 16-slot capacity was never checked kernel-side, only a much larger 64-slot bookkeeping table was. A caller submitting >16 outstanding commands without `Stream`'s userspace throttle could silently wrap `sq_tail` and overwrite a different session's descriptor. | Kernel (`../track-a/dma_accel_drv.c`) |
| M15 | M14 closed the corruption risk but not a fairness gap: one session filling the ring first could block every other session. Dynamic per-session quota (`ceil(16/active_sessions)`), enforced by blocking `ioctl(SUBMIT)` in the kernel - chosen so `Stream`'s already-frozen M10 contract needed zero code changes. | Kernel (`../track-a/dma_accel_drv.c`) |

**Kernel-side milestones live in `../track-a/dma_accel_drv.c`**, not in
this directory - session/queue state is part of the one driver module,
so those changes were made in place rather than as separate files. See
[`../track-a/README.md`](../track-a/README.md) for why. What's actually
in this directory:

```
track-b/
├── dma_accel_context.hpp / .cpp          M11 Context
├── dma_accel_context_smoke_test.cpp
├── dma_accel_scheduler.hpp / .cpp        M13 Scheduler
├── dma_accel_scheduler_test.cpp
├── dma_accel_session_isolation_test.cpp        M10.5 verification
├── dma_accel_completion_ownership_test.cpp     M10.5 verification
├── dma_accel_completion_leak_probe.cpp         M10.5 verification
├── dma_accel_resource_quota_test.cpp           M12 verification
├── dma_accel_sq_admission_test.cpp             M14 verification
└── dma_accel_fairness_quota_test.cpp           M15 verification
```

## Build

`Context`/`Scheduler` build on top of Track A's `Stream`/`Buffer`/`Fence`
(`../track-a/dma_accel_runtime.*`), so the include path needs both
directories:

```bash
cd track-b
g++ -std=c++17 -Wall -Wextra -c ../track-a/dma_accel_runtime.cpp -I ../track-a -o /tmp/runtime.o
g++ -std=c++17 -Wall -Wextra -c dma_accel_context.cpp -I ../track-a -o /tmp/context.o
g++ -std=c++17 -Wall -Wextra -c dma_accel_context_smoke_test.cpp -I ../track-a -o /tmp/context_smoke.o
g++ -std=c++17 -Wall -Wextra -o dma_accel_context_smoke_test /tmp/context_smoke.o /tmp/context.o /tmp/runtime.o
sudo ./dma_accel_context_smoke_test /dev/dma_accel0
```

The kernel module itself (Track A + Track B's kernel-side changes) is
built from `../track-a/` - see [`../track-a/README.md`](../track-a/README.md).
