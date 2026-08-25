// dma_accel_context.hpp
//
// M11: Context abstraction. Sits above Stream (M10), which itself wraps
// exactly one open /dev/dma_accel0 fd — a P1 "session" as of M10.5, which
// is the kernel's actual enforcement boundary for buffer and completion
// ownership (see M10.5-session-ownership.md). Context does not replace
// that enforcement; it adds two things Stream alone doesn't have:
//
//   1. A stable client identity (Context::name()) — a human-readable
//      label for whoever's using this Context, for logging/diagnostics
//      now and for P3's future per-client attribution work later. Stream
//      has no notion of "who" is using a given fd; Context does.
//   2. Userspace buffer-provenance checking. If application code passes
//      a Buffer that wasn't allocated through THIS Context into one of
//      its submit() calls, Context catches that in-process and throws
//      std::logic_error naming the Context and the offending buffer_id
//      — before any syscall happens. Without this, the same mistake
//      still fails (the kernel's M10.5 ownership check rejects it with
//      EPERM regardless), but as a std::system_error surfacing from
//      deep inside a syscall, with none of the applic-level context
//      (which Context, which Buffer) that made the mistake easy to
//      find. This is the "P2 defines the semantic layer, P1 enforces
//      the boundary" split established in the M10/M11 design
//      discussion, applied concretely.
//
// Design invariants (frozen for M11 — see the M11 design discussion
// before changing any of these):
//   - One Context wraps exactly one Stream (one fd). A Context owning
//     multiple Streams is explicitly NOT in scope here — nothing in the
//     current design needs it yet, and P1's buffer ownership is
//     enforced per-fd (M10.5), so multiple Streams under one logical
//     Context couldn't share buffers across those Streams without a
//     kernel-side change (relaxing ownership from "this fd" to "this
//     fd's group") that hasn't been designed, let alone built. Revisit
//     only when a real workload needs one client to span multiple fds.
//   - Context is move-only, same rationale as Stream/Buffer/Fence: it
//     represents unique ownership of one Stream's identity, and
//     allowing copies would let two Context objects claim the same
//     underlying fd with no clear owner.
//   - Context is a thin wrapper, not a new execution/scheduling layer.
//     It has no queue, no priority, no quota — those are M12 (Resource
//     Manager) and M13 (Scheduler) concerns, deliberately not pulled
//     forward here. Multiple Contexts today just mean multiple
//     independent fds, each fully subject to whatever the kernel/device
//     already enforces (M10.5 ownership, DMA_ACCEL_QUEUE_DEPTH, etc.) —
//     Context does not coordinate between them in any way yet.
//   - The buffer-provenance check is a courtesy for catching application
//     bugs early, not a security boundary. The kernel's per-session
//     ownership check (M10.5) is the actual boundary; this check exists
//     purely to make an application's own mistake easier to debug in
//     the same process, not to guard against anything adversarial.
//
// Deliberately NOT in M11 (see design discussion — add only when a real
// workload demands it): multi-Stream Context, cross-Context buffer
// sharing, resource quotas, scheduling/priority, kctl integration.

#ifndef DMA_ACCEL_CONTEXT_HPP
#define DMA_ACCEL_CONTEXT_HPP

#include <cstdint>
#include <string>
#include <unordered_set>

#include "dma_accel_runtime.hpp"

namespace dma_accel {

// A named client identity wrapping exactly one Stream. See the file
// comment above for what this adds over using Stream directly.
class Context {
public:
	explicit Context(std::string name, const char *device_path = "/dev/dma_accel0");

	Context(Context &&other) noexcept;
	Context &operator=(Context &&other) noexcept;
	Context(const Context &) = delete;
	Context &operator=(const Context &) = delete;
	~Context() = default;

	const std::string &name() const { return name_; }

	// Allocates a buffer through this Context's Stream and records its
	// buffer_id as belonging to this Context, so a later submit() call
	// can recognize it. Throws std::system_error on ioctl/mmap failure
	// (same as Stream::alloc_buffer() — this just adds bookkeeping on
	// top, it doesn't change the failure behavior).
	Buffer alloc_buffer(std::uint32_t size);

	// Same shape and behavior as Stream::submit()/submit_scale_add()/
	// submit_tile_matmul(), except every Buffer argument is checked
	// against this Context's own allocations first. Throws
	// std::logic_error (naming this Context and the offending
	// buffer_id) if any Buffer argument wasn't allocated through this
	// Context — see the file comment for why this check exists and
	// what it isn't. Otherwise throws std::system_error exactly as
	// Stream's underlying call would.
	Fence submit(std::uint32_t opcode, const Buffer &src, std::uint32_t src_offset,
		     const Buffer &dst, std::uint32_t dst_offset, std::uint32_t len);
	Fence submit_scale_add(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
				std::uint32_t b_offset, const Buffer &dst, std::uint32_t dst_offset,
				std::uint32_t len, float scalar);
	Fence submit_tile_matmul(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
				  std::uint32_t b_offset, const Buffer &c, std::uint32_t c_offset);

	// Pure passthrough to the underlying Stream — pump()/wait() operate
	// on Fences, which already carry their own identity independent of
	// which Context observes them, so there's nothing Context-specific
	// to check here.
	void pump() { stream_.pump(); }
	bool wait(Fence &fence, int timeout_ms = -1) { return stream_.wait(fence, timeout_ms); }

private:
	// Throws std::logic_error if buf wasn't allocated through this
	// Context. `what` names the calling operation (e.g. "submit",
	// "submit_scale_add") for a clearer error message.
	void check_owned(const Buffer &buf, const char *what) const;

	std::string name_;
	Stream stream_;
	// buffer_id is unique device-wide (dev->buffers[] is one global
	// table across every session — see dma_accel_drv.c), so a flat set
	// of ids here is sufficient with no risk of collision against a
	// different Context's ids, even though this Context can of course
	// only ever populate it with ids its own Stream allocated.
	std::unordered_set<std::uint32_t> owned_buffer_ids_;
};

} // namespace dma_accel

#endif // DMA_ACCEL_CONTEXT_HPP
