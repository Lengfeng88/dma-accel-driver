// dma_accel_command_group.cpp — see dma_accel_command_group.hpp for the
// design rationale.

#include "dma_accel_command_group.hpp"

#include <chrono>
#include <stdexcept>

// Same shim as dma_accel_runtime.cpp: dma_accel_regs.h mixes kernel-
// internal structs (bare u32/u64/__packed) with the uAPI ones, with no
// __KERNEL__ guard, so it isn't standalone-compilable in userspace
// without this. Needed here only for the DMA_ACCEL_OK constant used in
// aggregate status computation.
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
} // namespace
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace dma_accel {

CommandGroup::CommandGroup(Stream &stream) : stream_(&stream) {}

CommandGroup::CommandGroup(CommandGroup &&other) noexcept
	: stream_(other.stream_), fences_(std::move(other.fences_)), sealed_(other.sealed_) {
	other.stream_ = nullptr;
	other.sealed_ = false;
}

CommandGroup &CommandGroup::operator=(CommandGroup &&other) noexcept {
	if (this != &other) {
		stream_ = other.stream_;
		fences_ = std::move(other.fences_);
		sealed_ = other.sealed_;
		other.stream_ = nullptr;
		other.sealed_ = false;
	}
	return *this;
}

void CommandGroup::add(Fence &&fence) {
	if (sealed_) {
		throw std::logic_error(
			"CommandGroup::add() called after seal() — a sealed group's "
			"membership is fixed; create a new group for further commands");
	}
	fences_.push_back(std::move(fence));
}

void CommandGroup::seal() {
	// Idempotent by design — see header comment: callers shouldn't have
	// to track whether they already called this.
	sealed_ = true;
}

void CommandGroup::require_sealed_for_query() const {
	if (!sealed_) {
		throw std::logic_error(
			"CommandGroup::query()/wait() called before seal() — aggregate "
			"state is only meaningful once membership is fixed");
	}
}

GroupStatus CommandGroup::query() {
	require_sealed_for_query();
	stream_->pump();

	bool any_not_ready = false;
	bool any_failed = false;
	for (const auto &f : fences_) {
		if (!f.is_ready()) {
			any_not_ready = true;
			continue;
		}
		if (f.status() != DMA_ACCEL_OK) {
			any_failed = true;
		}
	}

	if (any_not_ready) {
		return GroupStatus::Pending;
	}
	return any_failed ? GroupStatus::SomeFailed : GroupStatus::AllOk;
}

GroupStatus CommandGroup::wait(int timeout_ms) {
	require_sealed_for_query();

	using clock = std::chrono::steady_clock;
	const bool has_deadline = timeout_ms >= 0;
	const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);

	for (;;) {
		// Scan for a not-yet-ready fence and any failure seen so far
		// among the ones that ARE ready. Every fence in this group
		// shares the same Stream's completion queue, so blocking via
		// Stream::wait() on any one not-ready fence also drains (and
		// therefore advances) every other fence in the group as a side
		// effect of the shared pump() it performs internally — no need
		// for CommandGroup to reimplement poll/timeout bookkeeping
		// itself.
		Fence *pending = nullptr;
		bool any_failed = false;
		for (auto &f : fences_) {
			if (!f.is_ready()) {
				pending = &f;
				break;
			}
			if (f.status() != DMA_ACCEL_OK) {
				any_failed = true;
			}
		}

		if (pending == nullptr) {
			return any_failed ? GroupStatus::SomeFailed : GroupStatus::AllOk;
		}

		int slice_timeout_ms = -1;
		if (has_deadline) {
			const auto remaining = deadline - clock::now();
			if (remaining <= std::chrono::milliseconds(0)) {
				return GroupStatus::Pending; // timed out
			}
			slice_timeout_ms = static_cast<int>(
				std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
		}

		if (!stream_->wait(*pending, slice_timeout_ms)) {
			return GroupStatus::Pending; // timed out
		}
		// `pending` is now ready (or wait() would have returned false
		// above); loop again to re-scan for the next not-ready fence,
		// if any.
	}
}

} // namespace dma_accel
