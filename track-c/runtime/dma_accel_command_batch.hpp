// dma_accel_command_batch.hpp
//
// C4: Command Construction & Batching.
//
// IMPORTANT — read before using this class: this is NOT kernel-level or
// hardware-level batch submission. Checked against the actual driver
// ABI (dma_accel_regs.h): DMA_ACCEL_IOC_SUBMIT is defined as
// _IOWR(DMA_ACCEL_IOC_MAGIC, 2, struct dma_accel_submit) — a single
// command per ioctl, no count field, no descriptor array. There is no
// batch ioctl. Adding one would mean extending Track A's register spec,
// driver, and QEMU device model — out of scope for Track C (see
// TRACK_C_RUNTIME_SPEC.md §1) and NOT done here, not even partially or
// speculatively in a way that assumes a future ABI shape.
//
// What CommandBatch actually provides, given that hard constraint:
//   - Multiple commands can be constructed (append()) before any of
//     them touch the driver.
//   - freeze() fixes their order — no further append() after this.
//   - submit() dispatches them, in that fixed order, through the
//     existing single-command Stream::submit()/submit_scale_add()/
//     submit_tile_matmul() interface — i.e. one ioctl per command,
//     exactly as calling submit() in a loop would do. submit() returns
//     a sealed CommandGroup (C1) over the resulting Fences.
//
// What CommandBatch explicitly does NOT provide (do not claim these
// anywhere — in code, comments, or docs — even loosely):
//   - fewer syscalls than an equivalent loop of Stream::submit() calls
//   - one-shot kernel submission
//   - atomic hardware submission (a partially-submitted batch is a real,
//     visible possible outcome if a later append in the sequence throws
//     mid-submit() — see submit()'s doc comment)
//   - reduced kernel entry overhead
//   - device-side batch execution
//
// If a real, measured workload later shows ioctl-per-command overhead is
// actually a bottleneck, that measurement is what justifies proposing a
// batch ioctl as a Track A extension request (§7) — CommandBatch's job
// right now is to make that overhead visible and measurable, not to
// paper over it with an API that implies it's already solved.
//
// Relationship to other Track C milestones:
//   - C1 CommandGroup: tracks *already-submitted* commands. CommandBatch
//     is the mirror image — it constructs commands *before* submission.
//     submit() is the handoff point: CommandBatch's job ends, and the
//     resulting CommandGroup is C1's aggregate over what just happened.
//   - C3 DependencyEngine: CommandBatch does not implement any
//     dependency logic of its own, and does not try to. If a batched
//     command needs to depend on something, that command doesn't belong
//     in a CommandBatch — build it via DependencyEngine::defer_submit*()
//     instead. These are deliberately not merged into one type; see the
//     C3 design discussion for why CommandGroup and dependency tracking
//     were already kept separate — the same reasoning applies here.
//
// Design invariants (frozen for C4 — see the C4 design discussion before
// changing any of these):
//   - Lifecycle: OPEN -> FROZEN -> SUBMITTED. append() after freeze(),
//     or submit() before freeze(), both throw std::logic_error. submit()
//     called a second time also throws — the commands have already been
//     dispatched to hardware; calling it again would duplicate real
//     device work, not just re-read cached state (unlike CommandGroup's
//     query()/wait(), which are safe to call repeatedly).
//   - freeze() is idempotent, matching CommandGroup::seal()'s precedent
//     from C1 — callers shouldn't have to track whether they already
//     called it.
//   - Buffers passed to append*() must outlive submit() — same
//     non-owning-reference convention used for Buffer in C3's
//     DependencyEngine, not a new risk category.
//   - Bound to one Stream, like CommandGroup/DependencyEngine — no
//     cross-Stream batching, consistent with the C1/C6 boundary.

#ifndef DMA_ACCEL_COMMAND_BATCH_HPP
#define DMA_ACCEL_COMMAND_BATCH_HPP

#include <functional>
#include <vector>

#include "dma_accel_command_group.hpp"
#include "dma_accel_runtime.hpp"

namespace dma_accel {

class CommandBatch {
public:
	explicit CommandBatch(Stream &stream) : stream_(&stream) {}

	CommandBatch(CommandBatch &&) noexcept = default;
	CommandBatch &operator=(CommandBatch &&) noexcept = default;
	CommandBatch(const CommandBatch &) = delete;
	CommandBatch &operator=(const CommandBatch &) = delete;

	// Queues a command for later submission, in call order. Throws
	// std::logic_error if already frozen. `src`/`dst` must outlive
	// submit() — see header comment.
	void append(std::uint32_t opcode, const Buffer &src, std::uint32_t src_offset, const Buffer &dst,
		    std::uint32_t dst_offset, std::uint32_t len);
	void append_scale_add(const Buffer &a, std::uint32_t a_offset, const Buffer &b, std::uint32_t b_offset,
			       const Buffer &dst, std::uint32_t dst_offset, std::uint32_t len, float scalar);
	void append_tile_matmul(const Buffer &a, std::uint32_t a_offset, const Buffer &b, std::uint32_t b_offset,
				 const Buffer &c, std::uint32_t c_offset);

	// Fixes command order; no more append() calls accepted afterward.
	// Idempotent.
	void freeze();
	bool is_frozen() const { return frozen_; }

	std::size_t size() const { return commands_.size(); }

	// Dispatches every queued command through Stream, one ioctl each,
	// in frozen order — see header comment for exactly what this does
	// and does not optimize. Throws std::logic_error if called before
	// freeze() or more than once (submitted_ is set before the loop
	// below starts, specifically so a second call after a mid-batch
	// failure refuses to retry rather than risking double-submitting
	// whatever already went through). If a command in the middle of
	// the sequence throws (e.g. std::system_error from the driver —
	// an out-of-bounds offset+len is a real example, per Stream::
	// submit()'s own doc comment in dma_accel_runtime.hpp), this is a
	// KNOWN, UNRECOVERED gap: commands submitted before the failure
	// have already reached hardware and will complete, but their
	// Fences are lost — nothing in this call returns them to the
	// caller, so they cannot be individually observed afterward (a
	// later Stream::pump()/wait() on this Stream will silently drain
	// their completions with no ill effect, just no visibility). This
	// is not hidden — it is the direct, honest consequence of
	// submit() returning one CommandGroup by value with no partial-
	// failure channel, and is left unsolved deliberately rather than
	// speculatively engineering recovery machinery nobody has asked
	// for yet. On success (no exception), returns a sealed
	// CommandGroup over every resulting Fence, in submission order.
	CommandGroup submit();

private:
	Stream *stream_;
	std::vector<std::function<Fence()>> commands_;
	bool frozen_ = false;
	bool submitted_ = false;
};

} // namespace dma_accel

#endif // DMA_ACCEL_COMMAND_BATCH_HPP
