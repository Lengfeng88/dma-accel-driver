// dma_accel_scheduler.hpp
//
// M13: Scheduler v0. Round-robin dispatch arbitration across multiple
// Context (M11) instances owned by ONE process.
//
// SCOPE — read before using this for anything cross-process:
// A Scheduler instance lives in one process's address space and only
// has visibility into Contexts it itself owns. It cannot, and does not
// attempt to, arbitrate submission order between two independent OS
// processes each holding their own fd — that would require either
// kernel-side SQ-level arbitration (a real change to
// dma_accel_ioc_submit()'s enqueue path, not designed) or an
// out-of-process broker every client funnels through (a materially
// different architecture, also not designed). Neither is built here.
// What M13 v0 actually solves: a single process managing several
// logical clients internally (each as its own Context) can interleave
// their command dispatch fairly, instead of the interleaving being
// whatever order the calling code happens to write submit() calls in.
// This is a real, common use case (e.g. one server process serving
// several sessions) — it is just not the same problem as fairness
// between unrelated processes, and this file does not claim to solve
// that.
//
// M10.5's ownership enforcement and M12's buffer quota both remain in
// effect regardless of process count — nothing about this scope
// limitation makes multi-process usage unsafe, only "not necessarily
// dispatch-order-fair across processes," which is a throughput/fairness
// concern, not a correctness one.
//
// What this is not, deliberately (see the M13 design discussion before
// changing any of these):
//   - Not priority-aware. Every registered Context gets equal turns in
//     round-robin order; there is no notion of one Context mattering
//     more than another. Add priority only when a real workload needs
//     it — it changes the arbitration algorithm, not just a parameter.
//   - Not kernel-side. Dispatch still goes through each Context's own
//     Stream::submit(), which still applies its own existing
//     per-fd backpressure (M10's DMA_ACCEL_QUEUE_DEPTH throttling) —
//     Scheduler does not need to reimplement or know about that; it
//     only decides ORDER, not admission.
//   - COPY only. enqueue_copy() is the only schedulable request shape
//     in this cut. submit_scale_add()/submit_tile_matmul() equivalents
//     would follow the identical pattern (same PendingCopy-shaped
//     struct, same dispatch loop) — deferred only because COPY alone
//     is sufficient to prove the round-robin property this milestone
//     is actually about; there's no technical obstacle to adding them.

#ifndef DMA_ACCEL_SCHEDULER_HPP
#define DMA_ACCEL_SCHEDULER_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "dma_accel_context.hpp"

namespace dma_accel {

class Scheduler {
public:
	Scheduler() = default;
	Scheduler(Scheduler &&) = default;
	Scheduler &operator=(Scheduler &&) = default;
	Scheduler(const Scheduler &) = delete;
	Scheduler &operator=(const Scheduler &) = delete;

	using ContextId = std::size_t;

	// Registers a Context with the scheduler, taking ownership of it —
	// consistent with Context/Stream/Buffer's existing move-only, RAII
	// style elsewhere in this runtime. Returns a stable id (just this
	// Context's index in registration order) used to enqueue work under
	// it and to identify it in run_round()'s results.
	ContextId add_context(Context ctx);

	// Reference to the underlying Context, e.g. to call
	// Context::name() for logging, or Context::alloc_buffer() before
	// enqueueing work against it. The Scheduler still owns it; this is
	// a non-owning view.
	Context &context(ContextId id) { return entries_.at(id).ctx; }

	// Enqueues one COPY request under the given Context's identity,
	// WITHOUT dispatching it yet — no ioctl(SUBMIT) happens here, only
	// bookkeeping. The actual submit() call (and with it, Context's
	// M11 buffer-provenance check) happens later, inside run_round().
	//
	// Precondition: src and dst must remain alive until this request is
	// actually dispatched by a run_round() call — same lifetime
	// contract Context::submit() already has for its Buffer arguments,
	// just deferred to whenever dispatch actually happens rather than
	// immediately. Scheduler stores non-owning pointers, not copies —
	// Buffer is move-only by design (see dma_accel_runtime.hpp), so
	// there is nothing else it could store.
	void enqueue_copy(ContextId id, const Buffer &src, std::uint32_t src_offset,
			   const Buffer &dst, std::uint32_t dst_offset, std::uint32_t len);

	// The actual scheduling decision. Dispatches every request that was
	// pending across every Context AT THE TIME THIS CALL STARTED, in
	// strict round-robin order: Context 0's oldest pending request,
	// Context 1's oldest, ..., Context N's oldest, then Context 0's
	// next, and so on — not "drain Context 0 entirely, then Context 1."
	// That interleaving is the concrete, testable property this
	// milestone is about: with A and B each having enqueued 3 requests,
	// dispatch order is A,B,A,B,A,B — not A,A,A,B,B,B.
	//
	// Requests enqueued by a caller *during* this call (not possible in
	// the current API surface, since there's no callback into user code
	// mid-dispatch, but worth being explicit about) would not be
	// included — only what was pending at call start.
	//
	// Returns (ContextId, Fence) pairs in actual dispatch order, so the
	// caller can correlate each completion back to which Context's
	// Buffers it touched. Each Context's own submit() throws exactly as
	// it would if called directly (std::logic_error for cross-Context
	// Buffer misuse per M11, std::system_error for anything the kernel
	// rejects) — a throw here aborts run_round() partway through,
	// leaving already-dispatched requests dispatched and the rest still
	// pending for a future call. No redundant validation happens in
	// Scheduler itself: Context::submit()'s existing M11 check already
	// runs at exactly the right time (dispatch, not enqueue), so
	// there's nothing for Scheduler to duplicate.
	std::vector<std::pair<ContextId, Fence>> run_round();

	// True if any registered Context has at least one undispatched
	// request. Convenience for driving run_round() in a loop.
	bool has_pending() const;

private:
	struct PendingCopy {
		const Buffer *src;
		std::uint32_t src_offset;
		const Buffer *dst;
		std::uint32_t dst_offset;
		std::uint32_t len;
	};
	struct Entry {
		explicit Entry(Context c) : ctx(std::move(c)) {}
		Context ctx;
		std::deque<PendingCopy> pending;
	};

	std::vector<Entry> entries_;
};

} // namespace dma_accel

#endif // DMA_ACCEL_SCHEDULER_HPP
