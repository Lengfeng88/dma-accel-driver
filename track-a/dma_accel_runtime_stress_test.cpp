// dma_accel_runtime_stress_test.cpp
//
// Exercises the SQ-depth backpressure Stream::submit() now enforces
// internally. DMA_ACCEL_QUEUE_DEPTH is 16 (dma_accel_regs.h); this
// submits well past that (40 commands, one buffer pair per command,
// fired in a tight loop with no manual waiting in between) specifically
// to prove the backpressure kicks in correctly instead of silently
// wrapping and corrupting the SQ ring. Before the backpressure fix, this
// exact pattern was the discovered hazard — a naive "submit everything,
// then wait on everything" GEMM tiling loop would have hit it.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_runtime_stress_test dma_accel_runtime_stress_test.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_runtime_stress_test [/dev/dma_accel0]

#include <cstdio>
#include <cstring>
#include <exception>
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

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";
	constexpr std::uint32_t kBufSize = 4096;
	constexpr int kNumCommands = 24; // past DMA_ACCEL_QUEUE_DEPTH (16); 2 buffers/cmd stays under MAX_BUFFERS (64)

	std::printf("== dma-accel M11-prep stress test: %d submits (queue depth is %d) ==\n",
		    kNumCommands, DMA_ACCEL_QUEUE_DEPTH);

	try {
		dma_accel::Stream stream(device);

		// One src/dst pair per command, each with a distinct pattern so
		// a mixed-up/overwritten descriptor shows up as a data
		// mismatch, not just a hang.
		std::vector<dma_accel::Buffer> srcs;
		std::vector<dma_accel::Buffer> dsts;
		std::vector<dma_accel::Fence> fences;
		srcs.reserve(kNumCommands);
		dsts.reserve(kNumCommands);
		fences.reserve(kNumCommands);

		for (int i = 0; i < kNumCommands; ++i) {
			srcs.push_back(stream.alloc_buffer(kBufSize));
			dsts.push_back(stream.alloc_buffer(kBufSize));

			const std::uint8_t pattern = static_cast<std::uint8_t>(i * 7 + 3);
			std::memset(srcs[i].data(), pattern, kBufSize);
			std::memset(dsts[i].data(), 0, kBufSize);
		}

		std::printf("submitting %d commands (no manual pacing — this is exactly what "
			    "would have wrapped the SQ ring before the backpressure fix)\n",
			    kNumCommands);
		for (int i = 0; i < kNumCommands; ++i) {
			// submit() itself may block here once 16 are outstanding —
			// that's the backpressure working as designed, not a bug.
			fences.push_back(stream.submit(OPCODE_COPY, srcs[i], 0, dsts[i], 0, kBufSize));
		}
		std::printf("all %d submitted; waiting on each\n", kNumCommands);

		bool ok = true;
		for (int i = 0; i < kNumCommands; ++i) {
			if (!stream.wait(fences[i], 5000)) {
				std::fprintf(stderr, "FAIL: command %d timed out\n", i);
				ok = false;
				continue;
			}
			if (fences[i].status() != DMA_ACCEL_OK) {
				std::fprintf(stderr, "FAIL: command %d status=%u\n", i, fences[i].status());
				ok = false;
				continue;
			}
			const std::uint8_t expected = static_cast<std::uint8_t>(i * 7 + 3);
			bool data_ok = true;
			for (std::uint32_t b = 0; b < kBufSize; ++b) {
				if (dsts[i].data()[b] != expected) {
					data_ok = false;
					break;
				}
			}
			if (!data_ok) {
				std::fprintf(stderr,
					     "FAIL: command %d data mismatch (expected pattern 0x%02x "
					     "throughout dst) — this is exactly the SQ-ring corruption "
					     "this test exists to catch\n",
					     i, expected);
				ok = false;
			}
		}

		if (!ok) {
			return 1;
		}
		std::printf("PASS: all %d commands completed with correct data, no SQ ring "
			    "corruption despite exceeding queue depth\n",
			    kNumCommands);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
