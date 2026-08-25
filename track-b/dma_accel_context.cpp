// dma_accel_context.cpp — see dma_accel_context.hpp for the design rationale.

#include "dma_accel_context.hpp"

#include <stdexcept>
#include <string>

namespace dma_accel {

Context::Context(std::string name, const char *device_path)
	: name_(std::move(name)), stream_(device_path) {}

Context::Context(Context &&other) noexcept
	: name_(std::move(other.name_)),
	  stream_(std::move(other.stream_)),
	  owned_buffer_ids_(std::move(other.owned_buffer_ids_)) {}

Context &Context::operator=(Context &&other) noexcept {
	if (this != &other) {
		name_ = std::move(other.name_);
		stream_ = std::move(other.stream_);
		owned_buffer_ids_ = std::move(other.owned_buffer_ids_);
	}
	return *this;
}

Buffer Context::alloc_buffer(std::uint32_t size) {
	Buffer buf = stream_.alloc_buffer(size);
	owned_buffer_ids_.insert(buf.buffer_id());
	return buf;
}

void Context::check_owned(const Buffer &buf, const char *what) const {
	if (owned_buffer_ids_.find(buf.buffer_id()) == owned_buffer_ids_.end()) {
		throw std::logic_error(
			"Context::" + std::string(what) + "(): buffer_id=" +
			std::to_string(buf.buffer_id()) + " was not allocated through Context '" +
			name_ +
			"' — this is an application bug (passing a Buffer across "
			"Contexts), not a runtime condition. The kernel would also "
			"reject this with EPERM (see M10.5-session-ownership.md); "
			"this check exists to catch it here first, with enough "
			"information to find the mistake.");
	}
}

Fence Context::submit(std::uint32_t opcode, const Buffer &src, std::uint32_t src_offset,
		       const Buffer &dst, std::uint32_t dst_offset, std::uint32_t len) {
	check_owned(src, "submit");
	check_owned(dst, "submit");
	return stream_.submit(opcode, src, src_offset, dst, dst_offset, len);
}

Fence Context::submit_scale_add(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
				 std::uint32_t b_offset, const Buffer &dst, std::uint32_t dst_offset,
				 std::uint32_t len, float scalar) {
	check_owned(a, "submit_scale_add");
	check_owned(b, "submit_scale_add");
	check_owned(dst, "submit_scale_add");
	return stream_.submit_scale_add(a, a_offset, b, b_offset, dst, dst_offset, len, scalar);
}

Fence Context::submit_tile_matmul(const Buffer &a, std::uint32_t a_offset, const Buffer &b,
				   std::uint32_t b_offset, const Buffer &c, std::uint32_t c_offset) {
	check_owned(a, "submit_tile_matmul");
	check_owned(b, "submit_tile_matmul");
	check_owned(c, "submit_tile_matmul");
	return stream_.submit_tile_matmul(a, a_offset, b, b_offset, c, c_offset);
}

} // namespace dma_accel
