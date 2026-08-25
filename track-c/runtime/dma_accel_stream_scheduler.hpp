// dma_accel_stream_scheduler.hpp
//
// C7: Execution Scheduling.
//
// Directly informed by C6a/C6b's evidence, not a speculative design:
//   - C6a (single thread, manually alternating submission A0,B0,A1,B1...)
//     achieved 7 cross-stream completion transitions out of 16 commands.
//   - C6b (two real OS threads, uncoordinated) achieved only 5.
// Deliberate, single-threaded round-robin submission ordering outperformed
// naive real concurrency. So C7 does NOT introduce more threading — it
// generalizes C6a's proven-better manual technique into a reusable,
// single-threaded scheduler. No new KCTL usage here: this milestone's
// mechanism is submission ORDER, not concurrent execution, and §6.5's
// KCTL evaluation was already done at C6b for the concurrency-shaped
// problem that actually existed there.
//
// C3/C7 boundary (frozen — see TRACK_C_RUNTIME_SPEC.md §5, restated
// here because it's directly relevant): C3's DependencyEngine answers
// "is this command eligible to submit yet?" C7 answers "given several
// eligible, ready-to-submit commands across multiple streams, in what
// order should they actually be presented to the driver?" C7 v1 does
// NOT integrate with C3's dependency graph — it schedules flat, already-
// eligible closures per stream. Combining dependency-awareness with
// scheduling is a real, larger capability with no demonstrated need yet;
// building it now would be speculative. If a workload needs both
// together, that need is what justifies the integration later.
//
// Design invariants (frozen for C7):
//   - Single-threaded, explicitly driven — run() is called by the
//     caller; no background thread, matching every other Track C
//     milestone's invariant inherited from M10's Fence/Stream.
//   - No client identity, no priority, no fairness weighting — pure
//     round-robin across registered streams, in registration order.
//     This is the "no policy" null case per §5's C3/C7 boundary note;
//     if a real workload needs weighted/priority scheduling, that's a
//     new, distinct capability to add later, not retrofitted here.
//   - Bound to the streams registered via add_stream(); each stream's
//     pending commands are plain std::function<Fence()> closures, same
//     convention as C3's PendingCommandState and C4's CommandBatch —
//     the caller is responsible for the same Buffer-outlives-submission
//     lifetime discipline already established for that pattern.
//   - Blind submission: does not query the driver's internal admission
//     state (Track B's per-session quota, MAX_INFLIGHT, etc.) before
//     submitting — submits per round-robin order and lets Stream::
//     submit()'s own backpressure (already present in M10) apply
//     naturally. Consistent with §5.3's original C7 note.

#ifndef DMA_ACCEL_STREAM_SCHEDULER_HPP
#define DMA_ACCEL_STREAM_SCHEDULER_HPP

#include <cstddef>
#include <deque>
#include <functional>
#include <vector>

#include "dma_accel_runtime.hpp"

namespace dma_accel {

// One command's completion, tagged with which registered stream (by
// registration index) it came from — lets a caller reconstruct which
// stream a given Fence belongs to after run() interleaves everything.
struct ScheduledResult {
	std::size_t stream_index;
	Fence fence;
};

// Round-robin submission scheduler across multiple Streams. See header
// comment for the full rationale, in particular why this is single-
// threaded rather than the multi-threaded direction C6b explored.
class StreamScheduler {
public:
	StreamScheduler() = default;
	StreamScheduler(StreamScheduler &&) = default;
	StreamScheduler &operator=(StreamScheduler &&) = default;
	StreamScheduler(const StreamScheduler &) = delete;
	StreamScheduler &operator=(const StreamScheduler &) = delete;

	// Registers a Stream (non-owning — caller keeps it alive for the
	// scheduler's lifetime, same convention as CommandGroup/
	// DependencyEngine elsewhere in Track C) and returns its index for
	// later reference in ScheduledResult. Order of add_stream() calls is
	// the round-robin order.
	std::size_t add_stream(Stream &stream);

	// Queues one deferred command for the given stream index. Throws
	// std::out_of_range if the index wasn't returned by add_stream().
	void enqueue(std::size_t stream_index, std::function<Fence()> submit_fn);

	// Round-robins through every registered stream's queue: one
	// submission from stream 0, then stream 1, ..., then back to stream
	// 0, until every queue is empty. A stream with an empty queue is
	// simply skipped that round, not treated as an error. Returns every
	// resulting Fence, tagged by stream index, in the order they were
	// actually submitted (i.e. the interleaved round-robin order, not
	// grouped by stream).
	std::vector<ScheduledResult> run();

private:
	std::vector<Stream *> streams_;
	std::vector<std::deque<std::function<Fence()>>> queues_;
};

} // namespace dma_accel

#endif // DMA_ACCEL_STREAM_SCHEDULER_HPP
