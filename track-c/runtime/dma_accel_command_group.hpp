// dma_accel_command_group.hpp
//
// C1: Multi-command Submission & Tracking.
//
// M10 (Track A, frozen — see dma_accel_runtime.hpp) proves a single
// command can be submitted and tracked correctly via Stream::submit() +
// Fence. C1 does not add "the ability to submit multiple commands" —
// that was already possible by calling submit() in a loop, as Track A's
// M11 tiled GEMM already does.
//
// What C1 actually adds: a first-class, trackable unit for a *set* of
// related commands — CommandGroup — with defined submission ordering and
// an explicit lifecycle, instead of leaving that bookkeeping to ad hoc
// caller-side loops and manually-managed vectors of Fence.
//
// Design invariants (frozen for C1 — see the C1 design discussion before
// changing any of these):
//   - CommandGroup is bound to exactly one Stream at construction and
//     tracks only commands submitted on that Stream. Cross-Stream
//     aggregation is explicitly out of scope for C1 — that is a C6
//     (multi-stream) concern. Introducing it here would blur the C1/C6
//     boundary.
//   - CommandGroup is a tracking / aggregation primitive, not a
//     submission-optimization primitive. It does not imply one syscall,
//     contiguous hardware submission, or reduced submission overhead.
//     That is what C4 (CommandBatch) is for — a deliberately separate
//     type. See the C1 design discussion for why these are not merged.
//   - Lifecycle is exactly three states:
//       OPEN    — commands may be add()-ed.
//       SEALED  — no more add(); aggregate state can be queried/waited.
//       (aggregate result: Pending / AllOk / SomeFailed, computed by
//        query()/wait(), not a fourth lifecycle state — there is no
//        background thread pushing completion; it's read on demand, same
//        as Fence/Stream in M10.)
//     add() after seal() throws std::logic_error. query()/wait() before
//     seal() throws std::logic_error. This mirrors Fence::status()'s
//     "wrong-time call is a caller bug, not a runtime condition" rule
//     from M10.
//   - Move-only, like Buffer/Fence/Stream: a CommandGroup represents a
//     specific set of in-flight commands: copying it would let two
//     objects claim ownership of the same tracked completions.
//   - Single-threaded, like the rest of the M10 runtime: query()/wait()
//     call into the bound Stream's pump(), which is not safe to call
//     concurrently from multiple threads on the same Stream.
//
// Explicitly NOT in C1 (see C1 design discussion — add only when a real
// workload demands it): batch submission, reduced syscall count,
// driver-level command lists (C4), dependency graphs between commands
// (C3), cross-Stream tracking (C6).

#ifndef DMA_ACCEL_COMMAND_GROUP_HPP
#define DMA_ACCEL_COMMAND_GROUP_HPP

#include <cstddef>
#include <vector>

#include "dma_accel_runtime.hpp"

namespace dma_accel {

// Aggregate result of a sealed CommandGroup, computed from the ready/
// status of every Fence currently in the group.
enum class GroupStatus {
	Pending,     // at least one command not yet ready
	AllOk,       // every command ready, every status() == DMA_ACCEL_OK
	SomeFailed,  // every command ready, at least one status() != DMA_ACCEL_OK
};

// Tracks a set of commands submitted on one Stream as a single logical
// execution unit. See the header comment above for the full design
// rationale and what this class deliberately does not do.
class CommandGroup {
public:
	// `stream` must outlive this CommandGroup. Mirrors the existing
	// Stream/Buffer lifetime relationship in dma_accel_runtime.hpp —
	// not re-explained here, see that header.
	explicit CommandGroup(Stream &stream);

	CommandGroup(CommandGroup &&other) noexcept;
	CommandGroup &operator=(CommandGroup &&other) noexcept;
	CommandGroup(const CommandGroup &) = delete;
	CommandGroup &operator=(const CommandGroup &) = delete;
	~CommandGroup() = default;

	// Takes ownership of `fence`. Throws std::logic_error if this group
	// is already sealed. Does not itself submit anything — the caller
	// submits via Stream::submit()/submit_scale_add()/submit_tile_matmul()
	// as usual and hands the resulting Fence in here; CommandGroup only
	// tracks, it never issues ioctls itself.
	void add(Fence &&fence);

	// Closes the group to further add() calls. Idempotent — calling
	// seal() on an already-sealed group is a no-op, not an error, so
	// callers don't need to track whether they've already called it.
	void seal();
	bool is_sealed() const { return sealed_; }

	// Number of commands currently tracked (both before and after seal).
	std::size_t size() const { return fences_.size(); }

	// Non-blocking: pumps the bound Stream once and returns the current
	// aggregate status. Throws std::logic_error if called before seal().
	GroupStatus query();

	// Blocks (via the bound Stream's poll/pump loop) until every
	// tracked command is ready, or timeout_ms elapses. timeout_ms < 0
	// waits indefinitely. Returns the aggregate status reached —
	// Pending only if the timeout elapsed first; otherwise AllOk or
	// SomeFailed. A timeout is an ordinary outcome (matches Stream::wait
	// in M10), not an exception. Throws std::logic_error if called
	// before seal().
	GroupStatus wait(int timeout_ms = -1);

	// Read-only access to the tracked fences, e.g. for per-command
	// diagnosis after wait() returns SomeFailed. Deliberately the only
	// way to inspect individual results — CommandGroup does not
	// duplicate per-field getters that Fence already provides.
	const std::vector<Fence> &fences() const { return fences_; }

private:
	void require_sealed_for_query() const;

	Stream *stream_;
	std::vector<Fence> fences_;
	bool sealed_ = false;
};

} // namespace dma_accel

#endif // DMA_ACCEL_COMMAND_GROUP_HPP
