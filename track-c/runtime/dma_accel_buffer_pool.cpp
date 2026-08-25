// dma_accel_buffer_pool.cpp — see dma_accel_buffer_pool.hpp for the
// design rationale and the corrected C5 scope.

#include "dma_accel_buffer_pool.hpp"

namespace dma_accel {

// ------------------------------------------------------------- PooledBuffer --

PooledBuffer::PooledBuffer(PooledBuffer &&other) noexcept
	: pool_(other.pool_), buffer_(other.buffer_) {
	other.pool_ = nullptr;
	other.buffer_ = nullptr;
}

PooledBuffer &PooledBuffer::operator=(PooledBuffer &&other) noexcept {
	if (this != &other) {
		release_if_held();
		pool_ = other.pool_;
		buffer_ = other.buffer_;
		other.pool_ = nullptr;
		other.buffer_ = nullptr;
	}
	return *this;
}

void PooledBuffer::release_if_held() {
	if (pool_ != nullptr && buffer_ != nullptr) {
		pool_->release(buffer_);
	}
	pool_ = nullptr;
	buffer_ = nullptr;
}

PooledBuffer::~PooledBuffer() { release_if_held(); }

// --------------------------------------------------------------- BufferPool --

BufferPool::BufferPool(Stream &stream, std::uint32_t buffer_size, std::uint32_t count) {
	// All real device allocations happen here, once, up front — this is
	// the only place this class ever calls Stream::alloc_buffer(). See
	// header comment: this is deliberate, it's the whole point.
	buffers_.reserve(count);
	for (std::uint32_t i = 0; i < count; ++i) {
		buffers_.push_back(stream.alloc_buffer(buffer_size));
	}
	free_list_.reserve(count);
	for (auto &b : buffers_) {
		free_list_.push_back(&b);
	}
}

std::optional<PooledBuffer> BufferPool::acquire() {
	if (free_list_.empty()) {
		return std::nullopt;
	}
	Buffer *b = free_list_.back();
	free_list_.pop_back();
	return PooledBuffer(this, b);
}

void BufferPool::release(Buffer *buffer) {
	free_list_.push_back(buffer);
}

} // namespace dma_accel
