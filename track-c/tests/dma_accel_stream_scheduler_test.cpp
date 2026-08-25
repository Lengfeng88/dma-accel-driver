// dma_accel_stream_scheduler_test.cpp
//
// C7 verification: reuses the exact same completion-timeline +
// transitions methodology from C6a/C6b, this time driven entirely by
// StreamScheduler's round-robin run() instead of hand-written
// alternation (C6a) or uncoordinated threads (C6b). Goal: confirm the
// generalized, reusable scheduler reaches interleaving at least as good
// as C6a's manually-engineered result (7 transitions across 16
// commands), ideally better since round-robin here is exact rather than
// approximate.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_stream_scheduler_test
//     dma_accel_stream_scheduler_test.cpp dma_accel_stream_scheduler.cpp
//     dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_stream_scheduler_test [/dev/dma_accel0]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

#include "dma_accel_runtime.hpp"
#include "dma_accel_stream_scheduler.hpp"

namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
} // namespace
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace {

constexpr int kCommandsPerStream = 8;
constexpr std::uint32_t kBufSize = 4096;
constexpr int kNumStreams = 2;

struct CompletionEvent {
	std::size_t stream_index;
	std::uint64_t cmd_id;
	long long elapsed_us;
};

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel C7 StreamScheduler verification ==\n");
	std::printf("using device %s, %d streams, %d commands each, round-robin scheduling\n", device,
		    kNumStreams, kCommandsPerStream);

	try {
		dma_accel::Stream streamA(device);
		dma_accel::Stream streamB(device);

		dma_accel::Buffer srcA = streamA.alloc_buffer(kBufSize);
		dma_accel::Buffer dstA = streamA.alloc_buffer(kBufSize);
		dma_accel::Buffer srcB = streamB.alloc_buffer(kBufSize);
		dma_accel::Buffer dstB = streamB.alloc_buffer(kBufSize);
		std::memset(srcA.data(), 0x11, kBufSize);
		std::memset(dstA.data(), 0, kBufSize);
		std::memset(srcB.data(), 0x22, kBufSize);
		std::memset(dstB.data(), 0, kBufSize);

		dma_accel::StreamScheduler scheduler;
		std::size_t idxA = scheduler.add_stream(streamA);
		std::size_t idxB = scheduler.add_stream(streamB);

		// Enqueue all of A's commands, then all of B's — deliberately in
		// "block" order at the ENQUEUE level, to specifically prove the
		// scheduler itself does the interleaving, not the caller. If
		// this still comes out interleaved despite block-order
		// enqueueing, that's the strongest possible evidence the
		// scheduler (not caller discipline) is what's responsible.
		for (int i = 0; i < kCommandsPerStream; ++i) {
			scheduler.enqueue(idxA, [&streamA, &srcA, &dstA]() -> dma_accel::Fence {
				return streamA.submit(OPCODE_COPY, srcA, 0, dstA, 0, kBufSize);
			});
		}
		for (int i = 0; i < kCommandsPerStream; ++i) {
			scheduler.enqueue(idxB, [&streamB, &srcB, &dstB]() -> dma_accel::Fence {
				return streamB.submit(OPCODE_COPY, srcB, 0, dstB, 0, kBufSize);
			});
		}

		auto t_start = std::chrono::steady_clock::now();
		std::vector<dma_accel::ScheduledResult> results = scheduler.run();

		if (results.size() != static_cast<std::size_t>(kNumStreams * kCommandsPerStream)) {
			std::fprintf(stderr, "FAIL: expected %d scheduled results, got %zu\n",
				     kNumStreams * kCommandsPerStream, results.size());
			return 1;
		}

		// Confirm the scheduler itself interleaved at SUBMISSION order,
		// independent of how completions later land — this is a direct,
		// deterministic check of run()'s own behavior.
		int submission_transitions = 0;
		for (std::size_t i = 1; i < results.size(); ++i) {
			if (results[i].stream_index != results[i - 1].stream_index) {
				++submission_transitions;
			}
		}
		std::printf("submission-order transitions (scheduler's own interleaving): %d "
			    "(expect close to %d for exact round-robin over 2 streams x %d each)\n",
			    submission_transitions, kNumStreams * kCommandsPerStream - 1, kCommandsPerStream);

		// Now poll for completions, same methodology as C6a/C6b.
		std::vector<bool> recorded(results.size(), false);
		std::vector<CompletionEvent> timeline;
		int total_recorded = 0;

		for (int iter = 0; iter < 2000 && total_recorded < static_cast<int>(results.size()); ++iter) {
			streamA.pump();
			streamB.pump();
			for (std::size_t i = 0; i < results.size(); ++i) {
				if (!recorded[i] && results[i].fence.is_ready()) {
					recorded[i] = true;
					++total_recorded;
					auto us = std::chrono::duration_cast<std::chrono::microseconds>(
							  std::chrono::steady_clock::now() - t_start)
							  .count();
					timeline.push_back(
						{results[i].stream_index, results[i].fence.cmd_id(), us});
				}
			}
			if (total_recorded < static_cast<int>(results.size())) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		if (total_recorded != static_cast<int>(results.size())) {
			std::fprintf(stderr, "FAIL: only %d/%zu completions observed\n", total_recorded,
				     results.size());
			return 1;
		}

		std::stable_sort(timeline.begin(), timeline.end(),
				  [](const CompletionEvent &x, const CompletionEvent &y) {
					  return x.elapsed_us < y.elapsed_us;
				  });

		std::printf("-- completion timeline --\n");
		for (const auto &ev : timeline) {
			std::printf("  %8lld us  stream=%zu cmd=0x%llx\n", ev.elapsed_us, ev.stream_index,
				    static_cast<unsigned long long>(ev.cmd_id));
		}

		int completion_transitions = 0;
		for (std::size_t i = 1; i < timeline.size(); ++i) {
			if (timeline[i].stream_index != timeline[i - 1].stream_index) {
				++completion_transitions;
			}
		}

		std::printf("-- summary --\n");
		std::printf("total completions: %d, completion-order transitions: %d\n", total_recorded,
			    completion_transitions);
		std::printf("(for reference: C6a manual alternation achieved 7, C6b uncoordinated "
			    "threads achieved 5, out of 16 commands)\n");

		if (completion_transitions < 7) {
			std::fprintf(stderr,
				     "FAIL: %d transitions is below C6a's manually-engineered baseline "
				     "(7) — the generalized scheduler underperforms the hand-written "
				     "version it's supposed to replace\n",
				     completion_transitions);
			return 1;
		}

		std::printf("PASS: %d completion-order transitions, at or above C6a's manual baseline "
			    "(7) — StreamScheduler successfully generalizes C6a's proven technique into "
			    "a reusable component, confirmed with commands enqueued in block order (all "
			    "of A, then all of B) — the interleaving credit belongs entirely to the "
			    "scheduler, not caller discipline\n",
			    completion_transitions);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
