// dma_accel_multistream_timeline_test.cpp
//
// C6a, take 2: the total-duration-ratio method
// (dma_accel_multistream_overlap_test.cpp /
// dma_accel_multistream_overlap_copy_test.cpp) turned out to be
// fundamentally ambiguous — a shared, fixed hardware concurrency budget
// (e.g. MAX_INFLIGHT commands device-wide, not per-session) would
// produce a ~2x ratio for 2x total commands REGARDLESS of whether the
// two sessions' commands were genuinely interleaved or fully serialized
// one-after-the-other. Total duration cannot distinguish those cases.
//
// This test instead records exactly when each command was submitted and
// exactly when each command's completion was first observed, for two
// independent Streams (A and B), then reports the chronological
// completion order directly — so the interleaving pattern (or lack of
// one) is visible, not inferred.
//
// Method: submit all of A's commands, then all of B's, without any
// waiting in between (so host-side submission order doesn't itself
// serialize anything). Then repeatedly pump() both streams (non-
// blocking, small sleep between iterations) and record the first
// instant each Fence transitions to ready. Report:
//   - the full chronological completion timeline (stream, cmd_id, us
//     since start)
//   - the number of A/B "transitions" in that order (adjacent completions
//     from different streams) — high transition count means fine-grained
//     interleaving; a transition count of 1 means strictly
//     "all of A, then all of B" (or vice versa) — genuine serialization
//   - explicit classification per the C6a acceptance criteria: this
//     test does NOT require a specific timing ratio or perfect
//     alternation; it requires that neither session is left waiting for
//     the other to fully drain before making any progress.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_multistream_timeline_test
//     dma_accel_multistream_timeline_test.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_multistream_timeline_test [/dev/dma_accel0]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

#include "dma_accel_runtime.hpp"

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

struct CompletionEvent {
	char stream_label;
	std::uint64_t cmd_id;
	long long elapsed_us;
};

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel C6a completion timeline test ==\n");
	std::printf("using device %s\n", device);

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

		auto t_start = std::chrono::steady_clock::now();

		std::vector<dma_accel::Fence> fencesA;
		std::vector<dma_accel::Fence> fencesB;
		fencesA.reserve(kCommandsPerStream);
		fencesB.reserve(kCommandsPerStream);

		std::printf("-- submit timeline --\n");
		for (int i = 0; i < kCommandsPerStream; ++i) {
			fencesA.push_back(streamA.submit(OPCODE_COPY, srcA, 0, dstA, 0, kBufSize));
			auto us = std::chrono::duration_cast<std::chrono::microseconds>(
					  std::chrono::steady_clock::now() - t_start)
					  .count();
			std::printf("  %8lld us  A submit cmd=0x%llx\n", static_cast<long long>(us),
				    static_cast<unsigned long long>(fencesA.back().cmd_id()));
		}
		for (int i = 0; i < kCommandsPerStream; ++i) {
			fencesB.push_back(streamB.submit(OPCODE_COPY, srcB, 0, dstB, 0, kBufSize));
			auto us = std::chrono::duration_cast<std::chrono::microseconds>(
					  std::chrono::steady_clock::now() - t_start)
					  .count();
			std::printf("  %8lld us  B submit cmd=0x%llx\n", static_cast<long long>(us),
				    static_cast<unsigned long long>(fencesB.back().cmd_id()));
		}

		// Poll both streams' completions without preferring either —
		// record the first instant each Fence transitions to ready.
		std::vector<bool> a_recorded(fencesA.size(), false);
		std::vector<bool> b_recorded(fencesB.size(), false);
		std::vector<CompletionEvent> timeline;
		int total_recorded = 0;
		const int total_expected = kCommandsPerStream * 2;

		for (int iter = 0; iter < 2000 && total_recorded < total_expected; ++iter) {
			streamA.pump();
			streamB.pump();

			for (std::size_t i = 0; i < fencesA.size(); ++i) {
				if (!a_recorded[i] && fencesA[i].is_ready()) {
					a_recorded[i] = true;
					++total_recorded;
					auto us = std::chrono::duration_cast<std::chrono::microseconds>(
							  std::chrono::steady_clock::now() - t_start)
							  .count();
					timeline.push_back({'A', fencesA[i].cmd_id(), us});
				}
			}
			for (std::size_t i = 0; i < fencesB.size(); ++i) {
				if (!b_recorded[i] && fencesB[i].is_ready()) {
					b_recorded[i] = true;
					++total_recorded;
					auto us = std::chrono::duration_cast<std::chrono::microseconds>(
							  std::chrono::steady_clock::now() - t_start)
							  .count();
					timeline.push_back({'B', fencesB[i].cmd_id(), us});
				}
			}

			if (total_recorded < total_expected) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		if (total_recorded < total_expected) {
			std::fprintf(stderr, "FAIL: only %d/%d completions observed before giving up\n",
				     total_recorded, total_expected);
			return 1;
		}

		std::stable_sort(timeline.begin(), timeline.end(),
				  [](const CompletionEvent &x, const CompletionEvent &y) {
					  return x.elapsed_us < y.elapsed_us;
				  });

		std::printf("-- completion timeline --\n");
		for (const auto &ev : timeline) {
			std::printf("  %8lld us  %c cmd=0x%llx\n", ev.elapsed_us, ev.stream_label,
				    static_cast<unsigned long long>(ev.cmd_id));
		}

		int transitions = 0;
		for (std::size_t i = 1; i < timeline.size(); ++i) {
			if (timeline[i].stream_label != timeline[i - 1].stream_label) {
				++transitions;
			}
		}

		bool strictly_serial = (transitions <= 1);

		std::printf("-- summary --\n");
		std::printf("total completions: %d, A/B transitions: %d\n", total_recorded, transitions);

		if (strictly_serial) {
			std::fprintf(stderr,
				     "FAIL (C6a not met): %d transition(s) — one session fully drained "
				     "before the other made any progress at all. This is genuine "
				     "cross-session serialization, not an artifact of the measurement.\n",
				     transitions);
			return 1;
		}

		std::printf("PASS (C6a met): %d transitions observed — neither session was required "
			    "to drain completely before the other progressed. This does not prove "
			    "physical execution-unit simultaneity (that would require device-model-level "
			    "instrumentation, out of scope here) — it proves genuine cross-session "
			    "interleaving at the runtime-observable completion level, which is C6a's "
			    "actual acceptance criterion.\n",
			    transitions);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
