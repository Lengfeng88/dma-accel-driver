// dma_accel_runtime.cpp — see dma_accel_runtime.hpp for the design rationale.

#include "dma_accel_runtime.hpp"

#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// Same shim as dma_accel_test.cpp: dma_accel_regs.h mixes kernel-internal
// structs (using bare u32/u64/__packed) with the actual uAPI ones
// (__u32/__u64), with no __KERNEL__ guard separating them, so it isn't
// standalone-compilable in userspace without this. See dma_accel_test.cpp
// for the fuller explanation; not repeating it at every include site
// beyond this pointer.
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
} // namespace
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace dma_accel {

namespace {

[[noreturn]] void throw_errno(const std::string &what) {
	throw std::system_error(errno, std::generic_category(), what);
}

} // namespace

// ---------------------------------------------------------------- Buffer --

Buffer::Buffer(std::uint8_t *addr, std::uint32_t size, std::uint32_t buffer_id)
	: addr_(addr), size_(size), buffer_id_(buffer_id) {}

Buffer::Buffer(Buffer &&other) noexcept
	: addr_(other.addr_), size_(other.size_), buffer_id_(other.buffer_id_) {
	other.addr_ = nullptr;
	other.size_ = 0;
}

Buffer &Buffer::operator=(Buffer &&other) noexcept {
	if (this != &other) {
		reset();
		addr_ = other.addr_;
		size_ = other.size_;
		buffer_id_ = other.buffer_id_;
		other.addr_ = nullptr;
		other.size_ = 0;
	}
	return *this;
}

void Buffer::reset() {
	// Unmaps the userspace virtual mapping only. The device-side
	// dma_alloc_coherent() allocation this mapping points at is NOT
	// freed here — see the header comment. It stays allocated until
	// the owning Stream's fd is closed (there is no BUFFER_FREE ioctl
	// in v0, per spec §11.2).
	if (addr_ != nullptr) {
		munmap(addr_, size_);
	}
	addr_ = nullptr;
	size_ = 0;
}

Buffer::~Buffer() { reset(); }

// ----------------------------------------------------------------- Fence --

std::uint32_t Fence::status() const {
	if (!state_->ready) {
		throw std::logic_error(
			"Fence::status() called before is_ready() — call Stream::wait() or "
			"Stream::pump() first");
	}
	return state_->status;
}

// ---------------------------------------------------------------- Stream --

Stream::Stream(const char *device_path) {
	fd_ = open(device_path, O_RDWR);
	if (fd_ < 0) {
		throw_errno(std::string("open(") + device_path + ")");
	}
}

Stream::Stream(Stream &&other) noexcept
	: fd_(other.fd_), outstanding_(std::move(other.outstanding_)) {
	other.fd_ = -1;
}

Stream &Stream::operator=(Stream &&other) noexcept {
	if (this != &other) {
		close_fd();
		fd_ = other.fd_;
		outstanding_ = std::move(other.outstanding_);
		other.fd_ = -1;
	}
	return *this;
}

void Stream::close_fd() noexcept {
	if (fd_ >= 0) {
		close(fd_);
		fd_ = -1;
	}
}

Stream::~Stream() { close_fd(); }

Buffer Stream::alloc_buffer(std::uint32_t size) {
	dma_accel_buffer_alloc req{};
	req.size = size;

	if (ioctl(fd_, DMA_ACCEL_IOC_BUFFER_ALLOC, &req) != 0) {
		throw_errno("ioctl(DMA_ACCEL_IOC_BUFFER_ALLOC)");
	}

	void *addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
			   static_cast<off_t>(req.mmap_offset));
	if (addr == MAP_FAILED) {
		throw_errno("mmap(buffer)");
	}

	return Buffer(static_cast<std::uint8_t *>(addr), size, req.buffer_id);
}

void Stream::throttle_before_submit() {
	// Backpressure: see the class-level comment in the header. The
	// driver's SQ ring has DMA_ACCEL_QUEUE_DEPTH slots and no
	// full-queue detection, so we must not have more than that many
	// outstanding before enqueuing one more. Must wait for the OLDEST
	// specifically — that's the ring slot the new submission would
	// otherwise silently overwrite.
	if (submission_order_.size() >= DMA_ACCEL_QUEUE_DEPTH) {
		FenceState &oldest = *submission_order_.front();
		if (!oldest.ready) {
			wait_state_blocking(oldest);
		}
		outstanding_.erase(oldest.cmd_id);
		submission_order_.pop_front();
	}
}

Fence Stream::do_submit(dma_accel_submit &req) {
	if (ioctl(fd_, DMA_ACCEL_IOC_SUBMIT, &req) != 0) {
		throw_errno("ioctl(DMA_ACCEL_IOC_SUBMIT)");
	}

	auto state = std::make_shared<FenceState>();
	state->cmd_id = req.cmd_id;
	outstanding_.emplace(req.cmd_id, state);
	submission_order_.push_back(state);
	return Fence(state);
}

Fence Stream::submit(std::uint32_t opcode, const Buffer &src, std::uint32_t src_offset,
		      const Buffer &dst, std::uint32_t dst_offset, std::uint32_t len) {
	throttle_before_submit();

	dma_accel_submit req{};
	req.opcode = opcode;
	req.len = len;
	req.src_buffer_id = src.buffer_id();
	req.src_offset = src_offset;
	req.dst_buffer_id = dst.buffer_id();
	req.dst_offset = dst_offset;
	// req.cmd_id is an out-param — the kernel allocates it (spec §11.1
	// decision 4); whatever we put in here going in is ignored.
	// req.src2_buffer_id / req.src2_offset / req.scalar stay zero —
	// COPY doesn't use them.

	return do_submit(req);
}

Fence Stream::submit_scale_add(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
				std::uint32_t b_offset, const Buffer &dst, std::uint32_t dst_offset,
				std::uint32_t len, float scalar) {
	throttle_before_submit();

	dma_accel_submit req{};
	req.opcode = OPCODE_SCALE_ADD;
	req.len = len;
	req.src_buffer_id = a.buffer_id();
	req.src_offset = a_offset;
	req.dst_buffer_id = dst.buffer_id();
	req.dst_offset = dst_offset;
	req.src2_buffer_id = b.buffer_id();
	req.src2_offset = b_offset;
	req.scalar = scalar;

	return do_submit(req);
}

Fence Stream::submit_tile_matmul(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
				  std::uint32_t b_offset, const Buffer &c, std::uint32_t c_offset) {
	throttle_before_submit();

	dma_accel_submit req{};
	req.opcode = OPCODE_TILE_MATMUL;
	req.len = DMA_ACCEL_TILE_MATMUL_BYTES;
	req.src_buffer_id = a.buffer_id();
	req.src_offset = a_offset;
	req.src2_buffer_id = b.buffer_id();
	req.src2_offset = b_offset;
	req.dst_buffer_id = c.buffer_id();
	req.dst_offset = c_offset;
	// req.scalar unused for TILE_MATMUL, stays zero.

	return do_submit(req);
}

void Stream::pump() {
	// Sized to drain a full SQ/CQ depth's worth (16, per
	// DMA_ACCEL_QUEUE_DEPTH) in one syscall in the common case; the
	// loop below handles however many are actually available regardless
	// of batch size.
	std::array<dma_accel_completion_uapi, 32> batch{};

	for (;;) {
		ssize_t n = read(fd_, batch.data(), sizeof(batch));
		if (n < 0) {
			if (errno == EAGAIN) {
				return; // nothing available right now — not an error
			}
			throw_errno("read(completion)");
		}
		if (n == 0) {
			return; // shouldn't happen per the driver's read() semantics, but don't spin
		}

		const std::size_t count = static_cast<std::size_t>(n) / sizeof(batch[0]);
		for (std::size_t i = 0; i < count; ++i) {
			auto it = outstanding_.find(batch[i].cmd_id);
			if (it == outstanding_.end()) {
				// A completion for a cmd_id this Stream doesn't know
				// about. Not expected to happen (every cmd_id we've
				// ever submitted goes into outstanding_ and is only
				// removed here), but not a reason to crash the
				// caller's whole pump() either — one stray completion
				// shouldn't stop the rest of the batch from being
				// processed.
				continue;
			}
			it->second->ready = true;
			it->second->status = batch[i].status;
			outstanding_.erase(it);
		}

		// A short read means the completion ring was drained to empty
		// at the moment of this read() — no need to loop again. A full
		// read means there may be more; go around and check.
		if (count < batch.size()) {
			return;
		}
	}
}

void Stream::wait_state_blocking(FenceState &state) {
	while (!state.ready) {
		pollfd pfd{};
		pfd.fd = fd_;
		pfd.events = POLLIN;

		int pret = poll(&pfd, 1, -1);
		if (pret < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw_errno("poll");
		}
		pump();
	}
}

bool Stream::wait(Fence &fence, int timeout_ms) {
	if (fence.is_ready()) {
		return true; // fast path — already satisfied, no syscalls needed
	}

	using clock = std::chrono::steady_clock;
	const bool has_deadline = timeout_ms >= 0;
	const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);

	while (!fence.is_ready()) {
		int poll_timeout = -1;
		if (has_deadline) {
			const auto remaining = deadline - clock::now();
			if (remaining <= std::chrono::milliseconds(0)) {
				return false;
			}
			poll_timeout = static_cast<int>(
				std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
		}

		pollfd pfd{};
		pfd.fd = fd_;
		pfd.events = POLLIN;

		int pret = poll(&pfd, 1, poll_timeout);
		if (pret < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw_errno("poll");
		}
		if (pret == 0) {
			return false; // timed out
		}

		pump();
	}

	return true;
}

} // namespace dma_accel
