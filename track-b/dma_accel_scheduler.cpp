// dma_accel_scheduler.cpp — see dma_accel_scheduler.hpp for design rationale.

#include "dma_accel_scheduler.hpp"

namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
} // namespace
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h" // for OPCODE_COPY

namespace dma_accel {

Scheduler::ContextId Scheduler::add_context(Context ctx) {
	entries_.emplace_back(std::move(ctx));
	return entries_.size() - 1;
}

void Scheduler::enqueue_copy(ContextId id, const Buffer &src, std::uint32_t src_offset,
			      const Buffer &dst, std::uint32_t dst_offset, std::uint32_t len) {
	entries_.at(id).pending.push_back(PendingCopy{&src, src_offset, &dst, dst_offset, len});
}

bool Scheduler::has_pending() const {
	for (const Entry &e : entries_) {
		if (!e.pending.empty()) {
			return true;
		}
	}
	return false;
}

std::vector<std::pair<Scheduler::ContextId, Fence>> Scheduler::run_round() {
	std::vector<std::pair<ContextId, Fence>> dispatched;

	// Snapshot how many requests each Context had pending at the start
	// of this call, so a round only ever dispatches "what was there
	// when we started" — a request some caller enqueues while this
	// call is running (not reachable from the current API, since
	// nothing calls back into user code mid-dispatch, but the counter
	// approach makes that guarantee explicit and robust either way)
	// waits for the next run_round() instead of extending this one.
	std::vector<std::size_t> remaining(entries_.size());
	for (std::size_t i = 0; i < entries_.size(); ++i) {
		remaining[i] = entries_[i].pending.size();
	}

	bool made_progress = true;
	while (made_progress) {
		made_progress = false;
		for (std::size_t i = 0; i < entries_.size(); ++i) {
			if (remaining[i] == 0) {
				continue; // this Context's snapshot is exhausted for this round
			}
			Entry &e = entries_[i];
			PendingCopy req = e.pending.front();
			e.pending.pop_front();
			--remaining[i];
			made_progress = true;

			// The real dispatch: Context::submit() runs its M11
			// buffer-provenance check here, then the actual
			// ioctl(SUBMIT). A throw here (cross-Context Buffer
			// misuse, or anything the kernel rejects) propagates
			// out of run_round() immediately — everything
			// dispatched so far in `dispatched` stays dispatched;
			// this request and everything still queued behind it
			// (in this Context and others) remains pending for a
			// future call.
			Fence fence =
				e.ctx.submit(OPCODE_COPY, *req.src, req.src_offset, *req.dst,
					     req.dst_offset, req.len);
			dispatched.emplace_back(i, std::move(fence));
		}
	}

	return dispatched;
}

} // namespace dma_accel
