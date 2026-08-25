// dma_accel_buffer_pool.hpp
//
// C5: Runtime Memory Management.
//
// Scope, corrected before implementation (not an oversight — see the C5
// design discussion): M10's Buffer has no BUFFER_FREE ioctl (deliberately
// deferred, per Track A's spec — "no workload has needed mid-session
// buffer release yet"). Once a buffer_id is allocated, it stays allocated
// until the owning Stream's fd closes. This removes two pieces of C5's
// originally-stated scope entirely:
//   - "Alignment" doesn't apply: Buffer's mapping is mmap-backed, already
//     page-aligned by construction, nothing for a pool to manage.
//   - "Fragmentation" doesn't apply in the traditional sense: there is no
//     free() to create holes, and no variable-size allocator here —
//     reuse is fixed-size slot cycling, not general allocation.
//
// What's left, and what's real: Track C's own C3 testing hit Track B's
// M12 per-session buffer quota (16 buffers/session, derived from the
// device's 4-way concurrency ceiling) simply because nothing was ever
// reused — every test allocated fresh buffers for every operation. This
// is the actual, demand-driven justification for C5: a fixed pool of
// pre-allocated, same-size buffers that get lent out and returned for
// reuse, so a workload with many sequential same-size operations doesn't
// linearly consume the session's buffer quota.
//
// Real correctness hazard this pool does NOT automatically prevent (the
// caller must uphold it, same non-owning-lifetime discipline already
// used throughout Track C): a PooledBuffer must not be returned to the
// pool (i.e. must not be destroyed / let go out of scope) while a
// command that references it is still in flight. Returning it early lets
// a later acquire() hand the same underlying buffer_id to an unrelated
// operation while the device may still be reading/writing it — a real
// data race, not a hypothetical one. Wait for the associated Fence to be
// ready before releasing.

#ifndef DMA_ACCEL_BUFFER_POOL_HPP
#define DMA_ACCEL_BUFFER_POOL_HPP

#include <cstdint>
#include <optional>
#include <vector>

#include "dma_accel_runtime.hpp"

namespace dma_accel {

class BufferPool;

// RAII handle to one pooled Buffer. Returns it to the pool's free list
// automatically on destruction — see the header comment above for the
// in-flight-command hazard this does NOT protect against; that's on the
// caller, not this class.
class PooledBuffer {
public:
	PooledBuffer(PooledBuffer &&other) noexcept;
	PooledBuffer &operator=(PooledBuffer &&other) noexcept;
	PooledBuffer(const PooledBuffer &) = delete;
	PooledBuffer &operator=(const PooledBuffer &) = delete;
	~PooledBuffer();

	// Pass-through convenience accessors — same shape as Buffer's own,
	// so a PooledBuffer can be used almost anywhere a Buffer reference
	// is expected (e.g. Stream::submit()'s `const Buffer&` parameters
	// via .buffer()).
	Buffer &buffer() { return *buffer_; }
	const Buffer &buffer() const { return *buffer_; }

private:
	friend class BufferPool;
	PooledBuffer(BufferPool *pool, Buffer *buffer) : pool_(pool), buffer_(buffer) {}
	void release_if_held();

	BufferPool *pool_;
	Buffer *buffer_; // non-owning — points into BufferPool's own storage
};

// Pre-allocates `count` buffers of `buffer_size`, once, at construction —
// this is where the real device allocations (and the session quota they
// consume) happen, up front, not per acquire(). See header comment for
// what this class does and does not solve.
class BufferPool {
public:
	BufferPool(Stream &stream, std::uint32_t buffer_size, std::uint32_t count);

	// Non-copyable, non-movable — PooledBuffer holds pointers into this
	// pool's own storage; moving the pool around adds a hazard for no
	// real benefit here, so it's ruled out entirely rather than reasoned
	// about case-by-case.
	BufferPool(const BufferPool &) = delete;
	BufferPool &operator=(const BufferPool &) = delete;
	BufferPool(BufferPool &&) = delete;
	BufferPool &operator=(BufferPool &&) = delete;

	// Returns nullopt if every buffer is currently checked out — an
	// ordinary, expected runtime condition (matches WaitResult::Pending's
	// precedent from C2), not an exception. The caller decides what to
	// do: wait for something to be released, grow demand elsewhere, etc.
	std::optional<PooledBuffer> acquire();

	std::size_t capacity() const { return buffers_.size(); }
	std::size_t available() const { return free_list_.size(); }
	std::size_t checked_out() const { return capacity() - available(); }

private:
	friend class PooledBuffer;
	void release(Buffer *buffer);

	std::vector<Buffer> buffers_;
	std::vector<Buffer *> free_list_;
};

} // namespace dma_accel

#endif // DMA_ACCEL_BUFFER_POOL_HPP
