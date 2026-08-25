# Track C — Accelerator Execution Runtime Capability Evolution

**Status:** Frozen baseline
**Depends on:** Track A M10 only (frozen snapshot). Does **not** depend on,
build on, or modify any Track B code.

---

## 0. Where Track C sits in the overall stack

```
                 DMA Accelerator
                       │
          ┌────────────┼────────────┐
          │            │            │
          ▼            ▼            ▼
       Track A       Track B       Track C
    "Make it       "Make it       "Make execution
     work"          safe/fair"     programmable
                                    and efficient"
          │            │            │
          ▼            ▼            ▼
       Driver       Isolation      Runtime
       DMA/IRQ      Ownership      Commands
       SQ/CQ        Quota          Async
       Compute      Fairness       Dependencies
       Recovery     Multi-client   Batching
                     (Context)     Memory
                                   Streams
                                   Scheduling
```

- **Track A** — hardware/driver bring-up. Can the accelerator be correctly
  controlled at all? (PCIe/MMIO, DMA+IRQ, SQ/CQ, compute opcodes, recovery.)
- **Track B** — system resource management. Is it safe and fair when
  multiple *identified clients* (`Context`) share the device within one
  process? (Session ownership, quota, Context-level round-robin, ring
  isolation, fairness.)
- **Track C** — execution runtime. Given hardware that already works, how
  is a single, possibly complex workload organized and executed
  efficiently — multi-command, async, dependencies, batching, memory
  reuse, multi-stream overlap, scheduling?

These are three different questions about the same accelerator, not three
revisions of the same feature. They are kept as three separate,
non-modifying code trees for exactly this reason — see §1 and §6.

---

## 1. Foundational rule: composition over modification

> Track C starts from a frozen snapshot of Track A M10. Track C does not
> modify Track A or Track B implementation files. New execution
> capabilities are implemented through new runtime components and
> composition over the frozen baseline.

Concretely:

- Track C **may use** Track A's `Buffer`, `Stream`, `Fence`, and the
  ioctl/mmap/poll/read ABI, exactly as frozen at Track A M10.
- Track C **may not modify** `dma_accel_drv.c`, `dma_accel_runtime.cpp`,
  `dma_accel_runtime.hpp`, or any other Track A file.
- Track C **does not use** Track B's `Context` or `Scheduler`. Track C
  streams are raw Track A streams with no identity concept — see §5.
- If a Track C capability genuinely cannot be built through composition
  and appears to require changing frozen Track A behavior, it is **not**
  quietly patched in. It is logged under §7 (Known Extension Requests)
  with: what's needed, why composition can't do it, and a decision
  (defer / accept as a documented, deliberate exception).

This keeps three independently readable engineering histories instead of
one line that keeps getting rewritten.

---

## 2. Directory layout

```
dma-accel/
│
├── track-a/
│   ├── dma_accel_drv.c
│   ├── dma_accel_runtime.cpp
│   ├── dma_accel_runtime.hpp
│   └── tests/
│
├── track-b/
│   ├── dma_accel_drv.c
│   ├── dma_accel_runtime.cpp
│   ├── dma_accel_runtime.hpp
│   ├── dma_accel_context.cpp
│   ├── dma_accel_scheduler.cpp
│   └── tests/
│
├── track-c/
│   ├── runtime/
│   │   ├── dma_accel_command.*
│   │   ├── dma_accel_dependency.*
│   │   ├── dma_accel_batch.*
│   │   ├── dma_accel_memory_pool.*
│   │   ├── dma_accel_multistream.*
│   │   └── dma_accel_executor.*
│   ├── tests/
│   └── docs/
│
└── docs/
```

`track-c/` imports Track A's frozen `Buffer`/`Stream`/`Fence` (via the
Track A headers, unmodified) and adds new components alongside them. No
file under `track-a/` is ever edited from within Track C work.

**Note (as of C0 verification):** the diagram above describes the
intended logical separation of the three tracks. As of C0, only
`track-c/` physically exists as an isolated directory — Track A and
Track B source currently live together, unsplit, in the guest VM project
root (`~/dma-accel-driver/`). This does not affect Track C's
composition-over-modification rule (§1): the files Track C imports were
verified byte-identical to their root-level originals at import time, and
nothing in the root is modified by Track C work. Whether to retroactively
split Track A/B into their own directories, or to treat only Track C as
requiring physical isolation (since it's the new, actively evolving
tree), is an open decision — see `C0-completion-note.md`.

---

## 3. Milestone sequence

```
Track C — Accelerator Execution Runtime
│
├── C0  Runtime MVP Baseline                    [frozen import, no new code]
│      Frozen snapshot of Track A M10
│      Buffer / Stream / Fence
│      Single-command execution
│
├── C1  Multi-command Submission                [DONE — verified 2026-08-20]
│      CommandGroup: tracks a set of commands submitted on one Stream
│      as a single logical unit (OPEN -> SEALED lifecycle, aggregate
│      Pending/AllOk/SomeFailed via query()/wait()). Explicitly not
│      batching (C4) and not cross-Stream (C6) — see
│      dma_accel_command_group.hpp and C1-completion-note.md.
│
├── C2  Observable Async Execution               [DONE — verified 2026-08-20]
│      StreamObserver: non-owning adapter over Stream's existing
│      pump()/wait() — adds query() (non-blocking) and
│      wait(Fence&, milliseconds) (timeout-bounded). Fence stays
│      passive (no Stream* back-reference, no Fence::query()/wait()).
│      WaitResult = {Completed, Pending} only — no Error variant;
│      genuine system errors still throw std::system_error, matching
│      M10's existing convention. See dma_accel_stream_observer.hpp
│      and C2-completion-note.md.
│
├── C3  Dependency & Synchronization             [DONE — verified 2026-08-20]
│      PendingCommand + DependencyEngine: host-side dependency tracking,
│      no device-side dependency ABI. Three depends_on() overloads with
│      different, individually-justified lifetime rules (Fence: move;
│      CommandGroup: non-owning; PendingCommand: safe shared_ptr copy).
│      DAGs supported, cycles rejected at depends_on() call time.
│      Failure propagates as permanent `blocked`. No scheduling policy —
│      FIFO is the null case, not a C7 decision. See
│      dma_accel_dependency.hpp and C3-completion-note.md (includes two
│      real bugs found during verification, both in the test harness).
│
├── C4  Command Construction & Batching          [DONE — verified 2026-08-20]
│      CommandBatch: OPEN -> FROZEN -> SUBMITTED lifecycle over a fixed,
│      ordered set of not-yet-submitted commands; submit() dispatches
│      via existing Stream calls (one ioctl each — NOT reduced syscall
│      count, see below) and returns a sealed CommandGroup. Scope was
│      explicitly corrected before implementation: dma_accel_regs.h has
│      no batch ioctl (single-command DMA_ACCEL_IOC_SUBMIT only), so
│      "submission overhead reduction" as originally worded is
│      undeliverable without extending Track A. Redefined to ordered
│      build/freeze/dispatch + automatic CommandGroup production, with
│      an explicit non-guarantee list (no fewer syscalls, no atomic
│      submission, no device-side batching). Has a known, documented,
│      unrecovered gap: a mid-batch ioctl failure loses the Fences of
│      already-submitted commands — verified, not hidden. See
│      dma_accel_command_batch.hpp and C4-completion-note.md.
│
├── C5  Runtime Memory Management                [DONE — verified 2026-08-20]
│      BufferPool + PooledBuffer: pre-allocates same-size buffers once,
│      RAII acquire/release for reuse. Scope corrected before
│      implementation — M10's Buffer has no BUFFER_FREE ioctl, so
│      "alignment" (mmap already page-aligns) and "fragmentation" (no
│      free() exists, nothing to fragment) don't apply. Real,
│      demand-driven justification: C3's own testing hit Track B's
│      16-buffer/session quota from never reusing buffers — C5 directly
│      relieves that, proven in testing (5 cycles through a 2-buffer
│      pool used exactly 2 buffer_ids, not 10). acquire() returns
│      std::optional, not an exception, on exhaustion. Pool is
│      non-movable (PooledBuffer holds pointers into its storage).
│      Still single-threaded — no KCTL involvement, per §6.5. See
│      dma_accel_buffer_pool.hpp and C5-completion-note.md.
│
├── C6  Multi-stream & Concurrent Execution
│      Split into two sub-milestones (see C6 design discussion):
│
│      C6a  Hardware overlap validation        [DONE — verified 2026-08-21]
│           Pure verification, no new production code — M10's Stream
│           already supports independent sessions. Real finding: total-
│           duration-ratio methodology is ambiguous (rejected after
│           testing); completion-timeline methodology showed block
│           submission (all of A, then all of B) dispatches strictly
│           serially (1 transition) because dispatch is FIFO-by-SQ-ring-
│           position with NO session-awareness — Track B's quota governs
│           admission, not dispatch order. Interleaved submission (A0,
│           B0, A1, B1...) confirmed the mechanism: 7 transitions,
│           genuine cross-session overlap. Conclusion: hardware overlap
│           is real but is a submission-order property, not automatic —
│           directly defines C7's job. See
│           dma_accel_multistream_timeline_interleaved_test.cpp and
│           C6a-completion-note.md.
│
│      C6b  Concurrent runtime execution        [DONE — verified 2026-08-21]
│           Two real OS threads, each exclusively driving its own Stream
│           (never shared, per M10's single-thread-per-Stream rule), no
│           artificial coordination between them. First real KCTL
│           integration: kctl::MpmcBoundedQueue used for lock-free
│           multi-producer completion-event reporting to the main
│           thread — a genuine fit, evaluated and justified.
│           kctl::ThreadPoolMpmc/Sharded and kctl::DeviceHandle were
│           evaluated and explicitly REJECTED with stated reasons (task-
│           pool shape mismatch; Stream already RAII-owns its fd).
│           Result: 5 cross-thread transitions across 16 completions —
│           genuine interleaving emerged from real concurrent submission
│           alone, without manual alternation, though less evenly than
│           C6a's deliberately-engineered 7 transitions — consistent
│           with C6a's conclusion that deliberate interleaving (C7's
│           job) still matters more than raw concurrency alone. See
│           dma_accel_multistream_threaded_test.cpp and
│           C6b-completion-note.md.
│
├── C7  Execution Scheduling                     [DONE — verified 2026-08-21]
│      StreamScheduler: exact round-robin submission across N Streams,
│      single-threaded (deliberately NOT more threading — C6a's manual
│      alternation (7/16 transitions) outperformed C6b's uncoordinated
│      real threads (5/16), so C7 generalizes C6a's approach rather than
│      pursuing more concurrency). Achieved 15/16 max-possible
│      transitions, confirmed even with block-order enqueueing (proving
│      the scheduler, not caller discipline, does the work). No
│      dependency-graph integration with C3 (scope boundary, stated
│      explicitly — no demonstrated need yet). No priority/fairness
│      policy — pure round-robin is the null case. No new KCTL usage
│      (mechanism is deterministic ordering, not concurrency). See
│      dma_accel_stream_scheduler.hpp and C7-completion-note.md.
│
└── C8  End-to-End Workload            [DONE — verified 2026-08-24]
       2-tile, K=3 TILE_MATMUL accumulation GEMM, verified against a CPU
       reference (final errors ~1e-6, within float32 rounding). Closes
       Track A's M11-deferred "hasn't yet been driven all the way
       through a full multi-tile GEMM." Exercises C1/C2/C3/C5/C6/C7
       together; C4 explicitly NOT exercised (no natural batch
       opportunity under the single-command ioctl ABI — independently
       validated separately). C3/C7 boundary explicitly preserved: two
       dependency chains driven by their own DependencyEngine.progress()
       loops (workload orchestration, not C7); C7 only schedules the
       final two flat, dependency-free validation COPYs. A real
       kernel-level deadlock (D-state, confirmed via ps/fuser) was found
       and traced to accumulated driver state from an extended,
       never-rmmod'd test session — resolved by a clean reboot, not a
       Track C code fix. See dma_accel_c8_workload.cpp and
       C8-completion-note.md.
```

Capability framing, one line per milestone:

| Milestone | Question it answers |
|---|---|
| C0 | Frozen starting point — what can Track A already do? |
| C1 | Can I execute many commands? |
| C2 | Can execution proceed asynchronously and be observed? |
| C3 | Can commands depend on each other? |
| C4 | Can I submit them efficiently? |
| C5 | Can I manage their memory efficiently? |
| C6 | Can independent work overlap? |
| C7 | Can I decide intelligently what to run next? |
| C8 | Can the whole thing execute a realistic workload? |

---

## 4. C0 — what "frozen snapshot" means

C0 is not a reimplementation and not new code. It is:

```
Track A M10
     │  frozen snapshot
     ▼
Track C C0
```

`track-c/` includes exactly four files, verbatim, from Track A's M10
checkpoint — nothing more:

```
dma_accel_runtime.hpp
dma_accel_runtime.cpp
dma_accel_runtime_smoke_test.cpp
dma_accel_regs.h        <- the driver ABI header; easy to miss because it
                            reads as "Track A driver," not "runtime," but
                            dma_accel_runtime.cpp and the smoke test both
                            hard-depend on it and won't build without it
```

C1 onward builds strictly on top of this checkpoint. Track A's later
milestones (M11 Tiled GEMM, compute opcodes work) are **not** pulled into
Track C's baseline; C0 is specifically the M10 runtime MVP checkpoint,
not "whatever Track A currently has."

**Verified 2026-08-20** — see `C0-completion-note.md` for the full
record: all four files diff-clean against Track A originals, build
zero-warning both in isolation and on the target guest VM, and the smoke
test passes against a real loaded driver + QEMU device
(`PASS: Stream/Buffer/Fence API verified, 4096 bytes copied correctly`),
matching Track A's original M10 verification behavior.

---

## 5. Boundary: Track B Scheduler vs Track C Scheduler

These solve genuinely different problems, not the same problem at
different granularity, because they operate on different objects:

```
Track B Scheduler                     Track C Scheduler (C7)
────────────────────                  ───────────────────────
Context A ─┐                          Stream A ─┐
Context B ─┼──► round-robin           Stream B ─┼──► execution
Context C ─┘    across identified              │    ordering policy
                 clients                Stream C ─┘   (no identity)
```

### 5.1 Track B Scheduler — Context-level fairness

Operates on `Context`, an identity-bearing abstraction (see Track B M11).
The question it answers: *given multiple named clients sharing this
process, how do we prevent one from starving another?* This is a fairness
problem in a multi-client setting.

### 5.2 Track C Scheduler (C7) — execution-level ordering

Operates on raw Track A `Stream`s with **no identity concept**, because
Track C does not use `Context`. The question it answers: *given one
logical workload that has organized its own work across multiple streams
for overlap/throughput, in what order should pending work be presented to
the driver for best execution efficiency?* This is a scheduling-for-
performance problem within a single logical user of the runtime, not a
fairness problem across separate users.

Because Track C never introduces client identity, it structurally cannot
overlap with what Track B's scheduler does — there is no "who" for C7 to
be fair or unfair to. This is the frozen boundary statement; if a future
milestone starts to need per-caller identity inside Track C, that is
itself a signal to stop and re-evaluate the boundary (see §7), not to
quietly grow one.

### 5.3 C7 admission behavior (unchanged from earlier draft)

C7 uses blind submission, not slot-aware submission: it does not query
Track A's driver for remaining `MAX_INFLIGHT` capacity before submitting.
It submits per its own ordering policy and reacts to backpressure/
rejection (retry, requeue, backoff) via the C2 observability primitives.
Track C never reads or assumes internal Track A admission state.

---

## 6. Why three separate code trees, not one evolving line

If Track C were built as `Track A → modify → Track B → modify → Track C`,
it would become difficult to answer, for any given piece of code, "which
architectural idea does this belong to?" Keeping three independent trees
means:

- Track A's code always represents *only* "can the hardware be controlled
  correctly."
- Track B's code always represents *only* "is it safe/fair for multiple
  identified clients."
- Track C's code always represents *only* "is a single workload executed
  efficiently."

Each tree stays legible as evidence of one specific engineering question,
which is also what makes the three-track structure useful for
architecture writeups and interview narratives: A / B / C map cleanly onto
three distinct systems problems solved on top of the same hardware.

---

## 6.5. KCTL Integration Boundary (frozen)

Track C composes over Track A's frozen `Buffer`/`Stream`/`Fence`, but it
does not use `kctl` (the completed, separately-verified C++ systems
library — RAII guards, lock-free SPSC/MPMC ring buffers, sharded thread
pools, polymorphic task queue) anywhere in C0–C5. This is a deliberate,
frozen architectural decision, not an oversight to be corrected by
retrofitting `kctl` into already-built milestones.

> **P2 owns execution semantics; KCTL supplies concurrency infrastructure
> only when those semantics require it.**

The reasoning:

- C0–C5 are, by design, single-threaded and explicitly progressed —
  inherited directly from M10's own frozen invariant on `Fence`/`Stream`
  ("single-threaded by design... nothing here is safe to call
  concurrently"). `CommandGroup::query()`/`wait()`, `StreamObserver`, and
  `DependencyEngine::progress()` all advance state only when explicitly
  called, with no background worker and no concurrent producer/consumer
  anywhere in the picture.
- `kctl`'s actual value — lock-free MPMC queues, thread pools, sharded
  execution — solves *concurrency* problems. None of C0–C5 has a genuine
  concurrency problem to solve. Introducing `kctl` primitives into any of
  them (e.g. routing `DependencyEngine`'s small, single-threaded DFS
  through a concurrent task queue) would be adopting machinery to serve
  an architecture diagram, not a real requirement — exactly the
  demand-driven violation this project has consistently avoided
  elsewhere.
- **C6 (Multi-stream & Overlap) is where a genuine concurrency
  requirement first appears**: multiple `Stream`s submitting and being
  tracked concurrently is a real producer/coordination problem, not a
  hypothetical one. At C6, `kctl`'s MPMC queues, thread-pool primitives,
  and related concurrency infrastructure will be evaluated against the
  actual multi-stream coordination requirements that emerge — some may
  fit directly, some may not (e.g. if a plain `std::mutex` turns out to
  be sufficient for what C6 actually needs, that's a legitimate outcome
  too; `kctl` is not owed a slot just because it exists).
- C7 (Scheduling) and C8 (Workload) build on whatever concurrency
  foundation C6 establishes; they are not independently re-evaluated for
  `kctl` fit before C6 exists.

Status table as of this freeze:

| Milestone | Genuine concurrency? | KCTL needed? |
|---|---|---|
| C0 | No | No |
| C1 CommandGroup | No | No |
| C2 Observable Async | No | No |
| C3 Dependency | No | No |
| C4 CommandBatch | No | No |
| C5 Memory Management | No (single-Stream design) | No |
| C6 Multi-stream | **Yes** | **First real evaluation point** |
| C7 Scheduling | Yes (built on C6) | Deferred to C6's outcome |
| C8 Workload | Yes (validation only) | Deferred to C6's outcome |

---

## 7. Known Extension Requests (log)

*(empty at freeze time — populate only if a genuine composition-blocking
need arises during C1–C8; each entry: capability needed, why composition
couldn't do it, decision and rationale)*

---

## 8. Cross-references

- Track A spec: `dma-accel-v0-register-spec.md` (frozen register/ABI
  source of truth).
- Track B docs: `docs/M10.5-session-ownership.md` through
  `docs/M15-fairness.md`.
- This document (`TRACK_C_RUNTIME_SPEC.md`) is the single source of truth
  for Track C milestone scope, ordering, and the layer boundaries in §5.
  Track B's scheduler doc should cross-reference §5 of this document, and
  vice versa, so the boundary is discoverable from either side.
