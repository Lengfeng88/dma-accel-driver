// dma_accel_command_batch.cpp — see dma_accel_command_batch.hpp for the
// design rationale, in particular the honest non-guarantee list and the
// known, unrecovered mid-batch-failure gap documented on submit().

#include "dma_accel_command_batch.hpp"

#include <stdexcept>

namespace dma_accel {

void CommandBatch::append(std::uint32_t opcode, const Buffer &src, std::uint32_t src_offset,
			   const Buffer &dst, std::uint32_t dst_offset, std::uint32_t len) {
	if (frozen_) {
		throw std::logic_error("CommandBatch::append() called after freeze()");
	}
	Stream *stream = stream_;
	const Buffer *src_p = &src;
	const Buffer *dst_p = &dst;
	commands_.push_back([stream, opcode, src_p, src_offset, dst_p, dst_offset, len]() -> Fence {
		return stream->submit(opcode, *src_p, src_offset, *dst_p, dst_offset, len);
	});
}

void CommandBatch::append_scale_add(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
				     std::uint32_t b_offset, const Buffer &dst, std::uint32_t dst_offset,
				     std::uint32_t len, float scalar) {
	if (frozen_) {
		throw std::logic_error("CommandBatch::append_scale_add() called after freeze()");
	}
	Stream *stream = stream_;
	const Buffer *a_p = &a;
	const Buffer *b_p = &b;
	const Buffer *dst_p = &dst;
	commands_.push_back([stream, a_p, a_offset, b_p, b_offset, dst_p, dst_offset, len, scalar]() -> Fence {
		return stream->submit_scale_add(*a_p, a_offset, *b_p, b_offset, *dst_p, dst_offset, len, scalar);
	});
}

void CommandBatch::append_tile_matmul(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
				       std::uint32_t b_offset, const Buffer &c, std::uint32_t c_offset) {
	if (frozen_) {
		throw std::logic_error("CommandBatch::append_tile_matmul() called after freeze()");
	}
	Stream *stream = stream_;
	const Buffer *a_p = &a;
	const Buffer *b_p = &b;
	const Buffer *c_p = &c;
	commands_.push_back([stream, a_p, a_offset, b_p, b_offset, c_p, c_offset]() -> Fence {
		return stream->submit_tile_matmul(*a_p, a_offset, *b_p, b_offset, *c_p, c_offset);
	});
}

void CommandBatch::freeze() {
	frozen_ = true; // idempotent — see header comment
}

CommandGroup CommandBatch::submit() {
	if (!frozen_) {
		throw std::logic_error("CommandBatch::submit() called before freeze()");
	}
	if (submitted_) {
		throw std::logic_error(
			"CommandBatch::submit() called more than once — commands from the first "
			"call may already be on hardware; refusing to risk double-submission");
	}
	// Set before the loop, not after — see header comment on why a
	// mid-batch failure must not leave the door open for a retry.
	submitted_ = true;

	CommandGroup group(*stream_);
	for (auto &fn : commands_) {
		// If fn() throws here, `group` (built so far) is destroyed as
		// the exception unwinds — see the KNOWN, UNRECOVERED gap
		// documented on submit() in the header. Not caught or papered
		// over here.
		group.add(fn());
	}
	group.seal();
	return group;
}

} // namespace dma_accel
