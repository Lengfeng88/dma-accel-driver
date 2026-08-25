// dma_accel_stream_observer.hpp
//
// C2: Observable Async Execution.
//
// M10 (Track A, frozen) already has basic async semantics: submit()
// returns immediately, Fence tracks completion, Stream::wait() blocks
// until ready or timeout. C2 does not add "asynchronous execution" —
// that already exists. What C2 adds: a non-blocking way to check
// progress (query()) alongside the existing blocking wait(), without
// modifying Fence or Stream, and without weakening M10's error-handling
// convention (genuine system errors throw; they are never folded into a
// return value).
//
// Architectural rule this class exists to demonstrate (see
// TRACK_C_RUNTIME_SPEC.md §1): Track C does not extend or modify frozen
// Track A classes. New capability is introduced through composition and
// a non-owning adapter over Stream's existing public API
// (Stream::pump(), Stream::wait(Fence&, int)) — not by adding methods to
// Stream itself. If StreamObserver were deleted entirely, Track A's M10
// baseline remains complete and unaffected.
//
// Design invariants for C2 (frozen for this milestone — see the C2
// design discussion before changing any of these):
//   - Fence stays exactly as M10 defined it: a passive completion-state
//     object. It does not gain a reference to Stream, and it does not
//     gain its own query()/wait(). Fence::is_ready() remains local,
//     cached-state-only, no I/O — that distinction from
//     StreamObserver::query() (which may pump the completion queue) is
//     the core thing C2 is establishing. See is_ready() vs query() below.
//   - StreamObserver is non-owning: it holds a Stream*, does not manage
//     its lifetime, and the caller is responsible for ensuring the
//     Stream outlives any StreamObserver built on it (same convention
//     CommandGroup already uses for its Stream reference).
//   - Unlike Buffer/Fence/Stream/CommandGroup, StreamObserver is
//     deliberately NOT move-only. Those types each represent a unique
//     ownership of something (a mapping, a command's completion
//     identity, an fd, a tracked command set) — copying them would let
//     two objects claim the same identity. StreamObserver owns nothing;
//     it is a view over a Stream, no different in kind from copying a
//     pointer. Default copy/move are used as-is.
//   - WaitResult has exactly two values: Completed and Pending. There is
//     no Error value. Verified against M10's actual Stream::wait()
//     implementation: poll()/read() failures already throw
//     std::system_error inside Stream — wait() returning false means
//     "timed out," never "something went wrong." StreamObserver does not
//     catch and repackage those exceptions; they propagate unchanged,
//     preserving M10's existing error-handling convention. Whether a
//     *completed* command itself succeeded or failed is a separate
//     question, orthogonal to WaitResult — check fence.status() after
//     Completed, exactly as CommandGroup checks status() after a fence
//     is ready (see dma_accel_command_group.hpp).
//
// Explicitly NOT in C2 (see design discussion — add only when a real
// workload demands it): callback/event notification, a background
// completion thread, modifying Stream or Fence, an Error variant that
// catches system exceptions.

#ifndef DMA_ACCEL_STREAM_OBSERVER_HPP
#define DMA_ACCEL_STREAM_OBSERVER_HPP

#include <chrono>

#include "dma_accel_runtime.hpp"

namespace dma_accel {

enum class WaitResult {
	Completed, // fence.is_ready() == true when this was returned
	Pending,   // still not ready — either this query found it not ready
	           // yet (non-blocking), or a bounded wait's timeout elapsed
	           // first. Either way: "not done, try again later." Does
	           // not mean anything went wrong — see class comment.
};

// Non-owning adapter that layers non-blocking and timeout-bounded
// observation on top of a Stream's existing public API. See the header
// comment above for the full rationale and what this class deliberately
// does not do.
class StreamObserver {
public:
	explicit StreamObserver(Stream &stream) : stream_(&stream) {}

	// Copy/move: defaulted, deliberately not move-only. See class
	// comment — this type owns nothing and has no identity to protect.

	// Non-blocking: pumps the bound Stream once (may perform I/O; may
	// throw std::system_error on a genuine transport failure, same as
	// Stream::pump() always could) and returns the fence's state
	// immediately after. Prefer this over calling fence.is_ready()
	// directly when you specifically want to give the completion queue
	// a chance to advance before checking — is_ready() alone never pumps
	// and will not reflect completions that arrived since the last pump.
	WaitResult query(Fence &fence);

	// Blocks until `fence` is ready or `timeout` elapses, whichever
	// comes first. Thin wrapper over Stream::wait(Fence&, int) — see
	// that method for the actual poll/pump loop. May throw
	// std::system_error on a genuine transport failure; never returns a
	// value to indicate one (see WaitResult's comment).
	WaitResult wait(Fence &fence, std::chrono::milliseconds timeout);

	// Blocks indefinitely until `fence` is ready. Always returns
	// Completed (barring an exception) — provided as a separate overload
	// rather than a magic negative-duration sentinel on the timeout
	// overload above, to keep "wait forever" an explicit choice at the
	// call site rather than an easy-to-miss default.
	WaitResult wait(Fence &fence);

private:
	Stream *stream_;
};

} // namespace dma_accel

#endif // DMA_ACCEL_STREAM_OBSERVER_HPP
