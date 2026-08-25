// dma_accel_runtime.hpp
//
// M10: P2 Accelerator Runtime skeleton. Lifts the raw ioctl/mmap/poll/read
// ABI that M9 validated (dma_accel_test.cpp) into a small C++ API:
//
//   Stream  — owns the device fd, owns completion processing (pump/wait)
//   Buffer  — owns a device buffer's userspace mapping (RAII munmap)
//   Fence   — identifies one submitted command's completion state
//
// Design invariants (frozen — see M10 design discussion before changing
// any of these):
//   - Fence is a plain state object, not a synchronizer: it holds no
//     mutex/condition_variable and starts no thread. All completion
//     processing happens inside Stream::pump(), called explicitly by
//     the caller (directly, or via Stream::wait()). This is single-
//     threaded by design — nothing here is safe to call concurrently
//     from multiple threads on the same Stream.
//   - Fence and Buffer are move-only: each represents a specific
//     command/buffer identity, and allowing copies would let two
//     objects claim the same identity with no clear owner.
//   - Buffer's destructor only munmaps the userspace mapping. The
//     device-side allocation is NOT freed here — per spec §11.2, there
//     is no BUFFER_FREE ioctl in v0; the accelerator buffer itself is
//     only released when the owning Stream's fd is closed. Letting a
//     Buffer outlive its Stream is safe (the mapping keeps the
//     underlying struct file alive), but the buffer stays allocated at
//     the device level until the Stream goes away regardless of when
//     any individual Buffer is destroyed.
//   - wait() returns bool (true = completed, false = timed out).
//     Timeout is an ordinary, expected outcome, not an error condition.
//     Genuine system errors (ioctl/read/poll failing, a completion for
//     an unknown cmd_id arriving from the driver — which should be
//     impossible but is worth a loud failure if it ever happens) throw
//     std::system_error / std::runtime_error.
//
// Deliberately NOT in M10 (see design discussion — add only when a real
// workload demands it): BUFFER_FREE, a background completion thread,
// coroutines/std::future, an executor, kctl.

#ifndef DMA_ACCEL_RUNTIME_HPP
#define DMA_ACCEL_RUNTIME_HPP

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

// Forward declaration only — the full definition (from dma_accel_regs.h)
// is an implementation detail pulled in by dma_accel_runtime.cpp, not
// exposed here. Only needed so Stream's private do_submit() helper below
// can take a reference to it in this header.
struct dma_accel_submit;

namespace dma_accel {

class Stream;

// One device buffer's userspace mapping. Obtained via Stream::alloc_buffer().
class Buffer {
public:
	Buffer(Buffer &&other) noexcept;
	Buffer &operator=(Buffer &&other) noexcept;
	Buffer(const Buffer &) = delete;
	Buffer &operator=(const Buffer &) = delete;
	~Buffer();

	std::uint8_t *data() const { return addr_; }
	std::uint32_t size() const { return size_; }
	std::uint32_t buffer_id() const { return buffer_id_; }

private:
	friend class Stream;
	Buffer(std::uint8_t *addr, std::uint32_t size, std::uint32_t buffer_id);
	void reset();

	std::uint8_t *addr_;
	std::uint32_t size_;
	std::uint32_t buffer_id_;
};

// Shared completion state for one submitted command. Stream::pump() writes
// into it; Fence::is_ready()/status() just read it. Not synchronized —
// only ever touched from the single thread driving this Stream.
struct FenceState {
	std::uint64_t cmd_id = 0;
	bool ready = false;
	std::uint32_t status = 0;
};

// Identifies one submitted command's completion. Move-only: a Fence is a
// unique handle on a specific cmd_id, not a copyable value.
class Fence {
public:
	Fence(Fence &&) noexcept = default;
	Fence &operator=(Fence &&) noexcept = default;
	Fence(const Fence &) = delete;
	Fence &operator=(const Fence &) = delete;

	std::uint64_t cmd_id() const { return state_->cmd_id; }
	bool is_ready() const { return state_->ready; }

	// Throws std::logic_error if called before is_ready() — reading a
	// completion status that hasn't happened yet is a caller bug, not
	// an ordinary runtime condition (unlike wait() timing out).
	std::uint32_t status() const;

private:
	friend class Stream;
	explicit Fence(std::shared_ptr<FenceState> state) : state_(std::move(state)) {}

	std::shared_ptr<FenceState> state_;
};

// Owns the device fd and all completion processing for it. Move-only: a
// Stream represents unique ownership of one open /dev/dma_accel0 fd.
//
// Enforces one invariant the driver itself does not: dma_accel_queue_submit()
// in the kernel has no full-SQ detection — submitting more than
// DMA_ACCEL_QUEUE_DEPTH (16, per dma_accel_regs.h) commands without any of
// them completing silently wraps the ring and overwrites an unconsumed
// descriptor (found while designing M11's tile-submission loop; the M6-M10
// self-tests never exceeded a handful of outstanding commands, so this never
// surfaced before). Stream::submit() applies backpressure instead of passing
// this hazard on to every caller: once DMA_ACCEL_QUEUE_DEPTH commands are
// outstanding, the next submit() blocks until the OLDEST of them completes
// (FIFO order, matching which ring slot is about to be reused) before
// enqueuing the new one.
class Stream {
public:
	explicit Stream(const char *device_path = "/dev/dma_accel0");

	Stream(Stream &&other) noexcept;
	Stream &operator=(Stream &&other) noexcept;
	Stream(const Stream &) = delete;
	Stream &operator=(const Stream &) = delete;
	~Stream();

	// Throws std::system_error on ioctl/mmap failure.
	Buffer alloc_buffer(std::uint32_t size);

	// Enqueues a COPY (or other opcode) command; returns immediately
	// (M9's SUBMIT ioctl is non-blocking — it returns once the
	// descriptor is enqueued, not once the DMA finishes). The kernel
	// assigns cmd_id; the Fence returned here is how you find out when
	// it's done. Throws std::system_error on ioctl failure (e.g. an
	// out-of-bounds src/dst_offset+len, an unknown buffer_id). May
	// block (see class comment) if DMA_ACCEL_QUEUE_DEPTH commands are
	// already outstanding — this is a real wait, not just a syscall,
	// so don't assume submit() is always cheap in a tight tiling loop.
	Fence submit(std::uint32_t opcode, const Buffer &src, std::uint32_t src_offset,
		     const Buffer &dst, std::uint32_t dst_offset, std::uint32_t len);

	// SCALE_ADD: out[i] = a[i]*scalar + b[i] over `len` bytes (must be a
	// multiple of sizeof(float) — the device validates this and returns
	// DMA_ACCEL_ERR_LENGTH via the Fence's status() otherwise, it isn't
	// checked here). A dedicated overload rather than folding into
	// submit() with optional parameters: SCALE_ADD's parameter shape
	// (two inputs + a scalar) is genuinely different from COPY's (one
	// input), and Buffer being move-only makes an "optional Buffer
	// reference" parameter awkward for no real benefit. Same blocking/
	// backpressure behavior as submit() — see the class comment.
	Fence submit_scale_add(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
				std::uint32_t b_offset, const Buffer &dst, std::uint32_t dst_offset,
				std::uint32_t len, float scalar);

	// TILE_MATMUL: c_tile += a_tile @ b_tile, fixed DMA_ACCEL_TILE_DIM
	// (32x32) square tiles — `len` isn't a parameter here because it's
	// always exactly one tile's worth (DMA_ACCEL_TILE_MATMUL_BYTES);
	// unlike SCALE_ADD, this isn't a variable-length op, so there's
	// nothing for the caller to specify. `c` is both read (the current
	// accumulator) and written (the new value) by the device — the
	// caller is responsible for zeroing it first if this is meant to be
	// the first term of a sum, not an accumulation onto stale data.
	Fence submit_tile_matmul(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
				  std::uint32_t b_offset, const Buffer &c, std::uint32_t c_offset);

	// Drains whatever completions are currently available (non-
	// blocking) and updates any matching outstanding Fences. Safe to
	// call speculatively; a no-op if nothing is ready yet. Throws
	// std::system_error if read() fails for a reason other than
	// "nothing available yet".
	void pump();

	// Blocks (via poll(), with the given timeout) until this specific
	// fence is ready, pumping as needed. timeout_ms < 0 means wait
	// indefinitely. Returns true if the fence completed, false if the
	// timeout elapsed first — a timeout is not an error.
	bool wait(Fence &fence, int timeout_ms = -1);

private:
	void close_fd() noexcept;
	// Blocks indefinitely (poll + pump in a loop) until state.ready is
	// true. Used both by the submit() backpressure path above and could
	// back wait() in the future if the two loops ever need to merge;
	// kept separate for now since wait() also needs timeout bookkeeping
	// this doesn't.
	void wait_state_blocking(FenceState &state);
	// Shared by submit() and submit_scale_add(): enforce the SQ-depth
	// bound before enqueuing (see class comment), then issue the ioctl
	// and start tracking the new command via a Fence. do_submit() takes
	// a mostly-filled-in request — callers set opcode/len/buffer fields
	// specific to what they're submitting; cmd_id is always an out-param.
	void throttle_before_submit();
	Fence do_submit(dma_accel_submit &req);

	int fd_ = -1;
	std::unordered_map<std::uint64_t, std::shared_ptr<FenceState>> outstanding_;
	// FIFO submission order, used only to find "the oldest outstanding
	// command" for the backpressure check above. outstanding_ (a hash
	// map) has no ordering; this does. Every entry here also has a copy
	// of the same shared_ptr in outstanding_.
	std::deque<std::shared_ptr<FenceState>> submission_order_;
};

} // namespace dma_accel

#endif // DMA_ACCEL_RUNTIME_HPP
