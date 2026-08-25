// dma_accel_dependency.cpp — see dma_accel_dependency.hpp for the design
// rationale.

#include "dma_accel_dependency.hpp"

#include <stdexcept>

// Same shim as the other Track C files — needed here for DMA_ACCEL_OK.
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

// True if `target` is reachable by following pending-command dependency
// edges starting from `from` (inclusive of `from` itself). Used by
// depends_on(const PendingCommand&) to reject an edge that would close a
// cycle — see the header comment for why this is checked here, at
// call-time, rather than discovered later as commands that silently
// never become eligible.
bool reachable(const std::shared_ptr<PendingCommandState> &from, const PendingCommandState *target) {
	if (from.get() == target) {
		return true;
	}
	for (const auto &dep : from->pending_deps) {
		if (reachable(dep, target)) {
			return true;
		}
	}
	return false;
}

} // namespace

// ---------------------------------------------------------- PendingCommand --

void PendingCommand::depends_on(Fence &&fence) {
	if (state_->submitted) {
		throw std::logic_error(
			"PendingCommand::depends_on() called after this command was already "
			"submitted — dependencies only make sense before submission");
	}
	state_->fence_deps.push_back(std::move(fence));
}

void PendingCommand::depends_on(const CommandGroup &group) {
	if (state_->submitted) {
		throw std::logic_error(
			"PendingCommand::depends_on() called after this command was already submitted");
	}
	state_->group_deps.push_back(&group);
}

void PendingCommand::depends_on(const PendingCommand &other) {
	if (state_->submitted) {
		throw std::logic_error(
			"PendingCommand::depends_on() called after this command was already submitted");
	}
	if (reachable(other.state_, state_.get())) {
		throw std::logic_error(
			"depends_on() rejected: this edge would create a dependency cycle "
			"(target is already reachable from the given dependency, including "
			"the self-dependency case)");
	}
	state_->pending_deps.push_back(other.state_);
}

Fence &PendingCommand::fence() {
	if (!state_->submitted) {
		throw std::logic_error("PendingCommand::fence() called before it has been submitted");
	}
	return *state_->fence;
}

// ---------------------------------------------------------- DependencyEngine --

PendingCommand DependencyEngine::defer_submit(std::uint32_t opcode, const Buffer &src,
					       std::uint32_t src_offset, const Buffer &dst,
					       std::uint32_t dst_offset, std::uint32_t len) {
	auto state = std::make_shared<PendingCommandState>();
	Stream *stream = stream_;
	const Buffer *src_p = &src;
	const Buffer *dst_p = &dst;
	state->submit_fn = [stream, opcode, src_p, src_offset, dst_p, dst_offset, len]() -> Fence {
		return stream->submit(opcode, *src_p, src_offset, *dst_p, dst_offset, len);
	};
	registry_.push_back(state);
	return PendingCommand(state);
}

PendingCommand DependencyEngine::defer_submit_scale_add(const Buffer &a, std::uint32_t a_offset,
							  const Buffer &b, std::uint32_t b_offset,
							  const Buffer &dst, std::uint32_t dst_offset,
							  std::uint32_t len, float scalar) {
	auto state = std::make_shared<PendingCommandState>();
	Stream *stream = stream_;
	const Buffer *a_p = &a;
	const Buffer *b_p = &b;
	const Buffer *dst_p = &dst;
	state->submit_fn = [stream, a_p, a_offset, b_p, b_offset, dst_p, dst_offset, len, scalar]() -> Fence {
		return stream->submit_scale_add(*a_p, a_offset, *b_p, b_offset, *dst_p, dst_offset, len, scalar);
	};
	registry_.push_back(state);
	return PendingCommand(state);
}

PendingCommand DependencyEngine::defer_submit_tile_matmul(const Buffer &a, std::uint32_t a_offset,
							    const Buffer &b, std::uint32_t b_offset,
							    const Buffer &c, std::uint32_t c_offset) {
	auto state = std::make_shared<PendingCommandState>();
	Stream *stream = stream_;
	const Buffer *a_p = &a;
	const Buffer *b_p = &b;
	const Buffer *c_p = &c;
	state->submit_fn = [stream, a_p, a_offset, b_p, b_offset, c_p, c_offset]() -> Fence {
		return stream->submit_tile_matmul(*a_p, a_offset, *b_p, b_offset, *c_p, c_offset);
	};
	registry_.push_back(state);
	return PendingCommand(state);
}

void DependencyEngine::progress() {
	stream_->pump();

	for (auto &state : registry_) {
		if (state->submitted || state->blocked) {
			continue;
		}

		bool any_pending = false;
		bool any_failed = false;

		for (const auto &f : state->fence_deps) {
			if (!f.is_ready()) {
				any_pending = true;
				break;
			}
			if (f.status() != DMA_ACCEL_OK) {
				any_failed = true;
				break;
			}
		}

		if (!any_pending && !any_failed) {
			for (const auto *g : state->group_deps) {
				for (const auto &f : g->fences()) {
					if (!f.is_ready()) {
						any_pending = true;
						break;
					}
					if (f.status() != DMA_ACCEL_OK) {
						any_failed = true;
						break;
					}
				}
				if (any_pending || any_failed) {
					break;
				}
			}
		}

		if (!any_pending && !any_failed) {
			for (const auto &dep : state->pending_deps) {
				if (dep->blocked) {
					any_failed = true;
					break;
				}
				if (!dep->submitted) {
					any_pending = true;
					break;
				}
				if (!dep->fence->is_ready()) {
					any_pending = true;
					break;
				}
				if (dep->fence->status() != DMA_ACCEL_OK) {
					any_failed = true;
					break;
				}
			}
		}

		if (any_failed) {
			state->blocked = true;
			continue;
		}
		if (any_pending) {
			continue; // not yet eligible — try again on a later progress() call
		}

		// All dependencies satisfied (or there were none) — submit now.
		state->fence = state->submit_fn();
		state->submitted = true;
	}
}

} // namespace dma_accel
