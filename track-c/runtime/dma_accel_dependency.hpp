// dma_accel_dependency.hpp
//
// C3: Dependency & Synchronization.
//
// Host-side runtime-managed dependency tracking. A command whose
// dependencies aren't yet satisfied is deferred rather than submitted
// immediately — this requires a placeholder for "a command that doesn't
// have a Fence yet, because it hasn't been submitted." That placeholder
// is PendingCommand; DependencyEngine owns the deferred-submission
// registry and is the only thing that actually calls into Stream.
//
// Scope, decided explicitly (not an oversight — see the C3 design
// discussion):
//   - Execution model is host-side only. There is no device-side
//     dependency ABI (dma_accel_regs.h's SQ descriptor has no field for
//     "depends on cmd_id X") and adding one would require extending
//     Track A's register spec and QEMU device model, which is out of
//     scope for Track C entirely — see TRACK_C_RUNTIME_SPEC.md §1.
//   - Dependency primitives: a bare Fence (ownership-transferred via
//     depends_on(Fence&&) — see below for why), a CommandGroup's
//     terminal fences (non-owning, same convention as elsewhere in
//     Track C), or another PendingCommand (a true DAG — see below).
//   - DAGs are supported; cycles are not, and are rejected at the
//     depends_on() call that would create one, not discovered later as
//     commands that silently never become eligible.
//   - Eligibility requires ALL dependencies to complete with
//     DMA_ACCEL_OK. A failed dependency permanently blocks the
//     dependent command — it is marked blocked, not left Pending
//     forever with no diagnostic.
//   - No scheduling policy: when a progress() call finds more than one
//     newly-eligible command, they submit in FIFO order by
//     PendingCommand registration order (registry order), full stop.
//     This is a default with no policy behind it, not a C7 decision
//     smuggled in early — see the C3/C7 boundary note below.
//
// C3/C7 boundary (frozen — do not blur this):
//   C3 answers "is this command eligible to submit yet?"
//   C7 answers "given several eligible commands and limited hardware
//   capacity, which should be submitted first?"
//   C3's FIFO default is not a scheduling policy in the C7 sense — it's
//   simply "no policy," the null case. If a real workload later needs
//   priority/fairness among eligible commands, that need is what
//   justifies building C7, not a reason to add policy logic here.
//
// Ownership/lifetime rules for depends_on() — three different rules for
// three different cases, each chosen for a specific reason:
//   - depends_on(Fence&&): ownership transfer (move). Fence has no
//     public accessor to its internal shared completion state (checked
//     against the actual M10 header — state_ is private, no friend
//     access outside Stream), so sharing ownership without modifying
//     Track A isn't possible. Moving the Fence in instead sidesteps the
//     lifetime question entirely: the caller's Fence variable is empty
//     after this call, so there is no other reference left that could
//     dangle. This mirrors CommandGroup::add(Fence&&)'s existing
//     precedent from C1. Tradeoff: the caller gives up independent
//     access to that Fence's status(). If you need both — as a
//     dependency AND independent inspection — put it in a CommandGroup
//     first and depend on the group instead.
//   - depends_on(const CommandGroup&): non-owning pointer. Same
//     established convention as Stream* inside CommandGroup/
//     StreamObserver — caller keeps the CommandGroup alive until this
//     dependency is resolved.
//   - depends_on(const PendingCommand&): safe shared_ptr copy of the
//     other command's internal state. No caller lifetime obligation at
//     all — DependencyEngine keeps every registered command's state
//     alive for the engine's own lifetime, so this is strictly safer
//     than the CommandGroup case, not just "another non-owning
//     reference to be careful with."
//
// Also worth flagging explicitly: defer_submit*() takes `const Buffer&`
// parameters the same way Stream::submit*() does, but here those Buffer
// references are captured (non-owning) inside a deferred submission
// closure — the caller must keep each Buffer alive not just until
// defer_submit*() returns, but until this PendingCommand actually
// submits, which may be arbitrarily delayed by its dependency chain.
// This is the same non-owning-reference convention used throughout
// Track C, just applied to Buffer for the first time — M10's Buffer
// itself is unmodified.

#ifndef DMA_ACCEL_DEPENDENCY_HPP
#define DMA_ACCEL_DEPENDENCY_HPP

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "dma_accel_command_group.hpp"
#include "dma_accel_runtime.hpp"

namespace dma_accel {

class DependencyEngine;

// Internal state shared between a PendingCommand handle and
// DependencyEngine's registry. Public fields, same transparency
// convention as M10's FenceState — not private/friended, but not meant
// to be poked at directly outside PendingCommand/DependencyEngine either.
struct PendingCommandState {
	std::function<Fence()> submit_fn; // captures Stream + opcode + Buffers
	std::vector<Fence> fence_deps;                     // owned (moved in)
	std::vector<const CommandGroup *> group_deps;      // non-owning
	std::vector<std::shared_ptr<PendingCommandState>> pending_deps; // safe shared ownership
	std::optional<Fence> fence; // set once submitted
	bool submitted = false;
	bool blocked = false; // a dependency failed — will never submit
};

// Handle to one deferred command. See the header comment above for the
// full design rationale, in particular the three different
// depends_on() lifetime rules.
class PendingCommand {
public:
	PendingCommand(PendingCommand &&) noexcept = default;
	PendingCommand &operator=(PendingCommand &&) noexcept = default;
	PendingCommand(const PendingCommand &) = delete;
	PendingCommand &operator=(const PendingCommand &) = delete;

	// Ownership transfer — see header comment. Throws std::logic_error
	// if this command has already been submitted (adding a dependency
	// after the fact makes no sense — it already ran).
	void depends_on(Fence &&fence);

	// Non-owning — group must outlive this dependency's resolution.
	// Same throw condition as above.
	void depends_on(const CommandGroup &group);

	// Safe shared_ptr copy — no caller lifetime obligation. Throws
	// std::logic_error if this command is already submitted, OR if
	// adding this edge would create a dependency cycle (checked here,
	// at call time, by walking `other`'s existing dependency graph —
	// see the .cpp for the reachability check).
	void depends_on(const PendingCommand &other);

	bool is_submitted() const { return state_->submitted; }
	bool is_blocked() const { return state_->blocked; }

	// Throws std::logic_error if !is_submitted().
	Fence &fence();

private:
	friend class DependencyEngine;
	explicit PendingCommand(std::shared_ptr<PendingCommandState> state)
		: state_(std::move(state)) {}

	std::shared_ptr<PendingCommandState> state_;
};

// Owns the deferred-submission registry for one Stream. See the header
// comment for the full design rationale.
class DependencyEngine {
public:
	explicit DependencyEngine(Stream &stream) : stream_(&stream) {}

	// Buffers passed here must outlive this PendingCommand's eventual
	// submission — see header comment. Does not submit anything itself;
	// only registers the deferred submission.
	PendingCommand defer_submit(std::uint32_t opcode, const Buffer &src, std::uint32_t src_offset,
				     const Buffer &dst, std::uint32_t dst_offset, std::uint32_t len);
	PendingCommand defer_submit_scale_add(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
					       std::uint32_t b_offset, const Buffer &dst,
					       std::uint32_t dst_offset, std::uint32_t len, float scalar);
	PendingCommand defer_submit_tile_matmul(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
						 std::uint32_t b_offset, const Buffer &c,
						 std::uint32_t c_offset);

	// Pumps the bound Stream once, then makes one pass over the
	// registry in FIFO registration order: for each not-yet-submitted,
	// not-blocked command, checks whether all of its dependencies
	// (fence_deps, group_deps, pending_deps) are complete with
	// DMA_ACCEL_OK. If so, actually calls into Stream to submit it. If
	// any dependency completed with a failing status, marks this
	// command blocked instead. A dependency chain longer than what a
	// single FIFO pass resolves may need more than one progress() call
	// to fully cascade — no different in spirit from needing multiple
	// pump()/wait() calls elsewhere in this runtime.
	void progress();

private:
	Stream *stream_;
	std::vector<std::shared_ptr<PendingCommandState>> registry_;
};

} // namespace dma_accel

#endif // DMA_ACCEL_DEPENDENCY_HPP
