// dma_accel_multistream_overlap_test.cpp
//
// C6a: Multi-stream hardware overlap verification.
//
// This is deliberately NOT new Track C production code — see the C6
// design discussion. C6a's whole point is that M10's Stream already
// supports multiple independent sessions with no changes needed; this
// test exists to prove, with real timing data, that two independent
// Streams' commands actually overlap at the hardware level (exploiting
// the device's MAX_INFLIGHT concurrency, which Track A's M7 self-test
// already validated at the driver level) — not that anything new needed
// to be built to make that true.
//
// Method: measure wall-clock time for K TILE_MATMUL commands on a single
// Stream (baseline, T_single), then measure wall-clock time for two
// independent Streams each submitting the same K commands, submitted
// before either is waited on (T_concurrent). If commands only ever ran
// serially across sessions, T_concurrent would be roughly 2x T_single.
// If they genuinely overlap in hardware, T_concurrent should be much
// closer to T_single than to 2x T_single.
//
// All commands reuse the same 3 buffers within each stream (this test
// only cares about timing, not data correctness per command) — this
// deliberately stays well under Track B's 16-buffer/session quota and
// avoids the topic entirely; a single TILE_MATMUL tile is exactly
// DMA_ACCEL_TILE_MATMUL_BYTES (4096 bytes for the 32x32 float32 tile).
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_multistream_overlap_test
//     dma_accel_multistream_overlap_test.cpp dma_accel_command_group.cpp
//     dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_multistream_overlap_test [/dev/dma_accel0]

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

#include "dma_accel_command_group.hpp"
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

constexpr int kCommandsPerStream = 8; // well under DMA_ACCEL_QUEUE_DEPTH (16)

// Submits `count` TILE_MATMUL commands on `stream` (reusing one buffer
// triple), sealing and waiting on a CommandGroup. Returns the wall-clock
// duration and whether every command completed OK.
std::pair<std::chrono::milliseconds, bool> run_tile_matmuls(dma_accel::Stream &stream, int count) {
	dma_accel::Buffer a = stream.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
	dma_accel::Buffer b = stream.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
	dma_accel::Buffer c = stream.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
	std::memset(a.data(), 0x11, DMA_ACCEL_TILE_MATMUL_BYTES);
	std::memset(b.data(), 0x22, DMA_ACCEL_TILE_MATMUL_BYTES);
	std::memset(c.data(), 0, DMA_ACCEL_TILE_MATMUL_BYTES);

	dma_accel::CommandGroup group(stream);

	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < count; ++i) {
		group.add(stream.submit_tile_matmul(a, 0, b, 0, c, 0));
	}
	group.seal();
	dma_accel::GroupStatus status = group.wait(5000);
	auto t1 = std::chrono::steady_clock::now();

	bool ok = (status == dma_accel::GroupStatus::AllOk);
	auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
	return {duration, ok};
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel C6a multi-stream hardware overlap test ==\n");
	std::printf("using device %s\n", device);

	try {
		// -- Baseline: one Stream, K commands, sequential-from-the-runtime's-
		// perspective (single session driving the SQ alone). Scoped in its
		// own block so the Stream (and its session) is fully destroyed
		// before the concurrent phase starts below — otherwise Track B's
		// M12/M15 per-session quota (ceil(16/active_sessions)) would see 3
		// active sessions during the concurrent phase instead of 2,
		// shrinking each session's fair-share admission quota and
		// potentially causing kernel-side ioctl(SUBMIT) blocking that has
		// nothing to do with actual hardware execution concurrency.
		std::chrono::milliseconds t_single{};
		{
			std::printf("-- baseline: single Stream, %d TILE_MATMUL commands --\n",
				    kCommandsPerStream);
			dma_accel::Stream baseline_stream(device);
			auto [duration, ok_single] = run_tile_matmuls(baseline_stream, kCommandsPerStream);
			if (!ok_single) {
				std::fprintf(stderr, "FAIL: baseline commands did not all complete OK\n");
				return 1;
			}
			t_single = duration;
			std::printf("T_single = %lldms\n", static_cast<long long>(t_single.count()));
		} // baseline_stream destroyed here — session fully closed

		// -- Concurrent: two independent Streams (sessions), each
		// submitting K commands before either is waited on. --
		std::printf("-- concurrent: two independent Streams, %d commands each --\n", kCommandsPerStream);
		dma_accel::Stream streamA(device);
		dma_accel::Stream streamB(device);

		dma_accel::Buffer aA = streamA.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		dma_accel::Buffer bA = streamA.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		dma_accel::Buffer cA = streamA.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		dma_accel::Buffer aB = streamB.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		dma_accel::Buffer bB = streamB.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		dma_accel::Buffer cB = streamB.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		std::memset(aA.data(), 0x11, DMA_ACCEL_TILE_MATMUL_BYTES);
		std::memset(bA.data(), 0x22, DMA_ACCEL_TILE_MATMUL_BYTES);
		std::memset(cA.data(), 0, DMA_ACCEL_TILE_MATMUL_BYTES);
		std::memset(aB.data(), 0x33, DMA_ACCEL_TILE_MATMUL_BYTES);
		std::memset(bB.data(), 0x44, DMA_ACCEL_TILE_MATMUL_BYTES);
		std::memset(cB.data(), 0, DMA_ACCEL_TILE_MATMUL_BYTES);

		dma_accel::CommandGroup groupA(streamA);
		dma_accel::CommandGroup groupB(streamB);

		auto t0 = std::chrono::steady_clock::now();
		// Submit ALL of A's commands, then ALL of B's, before waiting on
		// either — this is what actually tests overlap: if execution were
		// serial across sessions, B's commands would sit fully queued
		// behind A's before any hardware time is spent on them.
		for (int i = 0; i < kCommandsPerStream; ++i) {
			groupA.add(streamA.submit_tile_matmul(aA, 0, bA, 0, cA, 0));
		}
		for (int i = 0; i < kCommandsPerStream; ++i) {
			groupB.add(streamB.submit_tile_matmul(aB, 0, bB, 0, cB, 0));
		}
		groupA.seal();
		groupB.seal();
		dma_accel::GroupStatus statusA = groupA.wait(5000);
		dma_accel::GroupStatus statusB = groupB.wait(5000);
		auto t1 = std::chrono::steady_clock::now();

		if (statusA != dma_accel::GroupStatus::AllOk || statusB != dma_accel::GroupStatus::AllOk) {
			std::fprintf(stderr, "FAIL: concurrent commands did not all complete OK\n");
			return 1;
		}

		auto t_concurrent = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
		std::printf("T_concurrent = %lldms (2 streams x %d commands each = %d total commands)\n",
			    static_cast<long long>(t_concurrent.count()), kCommandsPerStream,
			    kCommandsPerStream * 2);

		double ratio = t_single.count() > 0
					? static_cast<double>(t_concurrent.count()) / static_cast<double>(t_single.count())
					: 0.0;
		std::printf("ratio T_concurrent / T_single = %.2f\n", ratio);
		std::printf("(serial-across-sessions would predict ~2.0; genuine hardware overlap predicts "
			    "well under 2.0, closer to 1.0)\n");

		if (ratio >= 1.5) {
			std::fprintf(stderr,
				     "FAIL: ratio %.2f >= 1.5 — commands do not appear to be overlapping "
				     "at the hardware level; sessions may be executing serially\n",
				     ratio);
			return 1;
		}

		std::printf("PASS: ratio %.2f < 1.5 — two independent Streams' commands genuinely "
			    "overlapped at the hardware level, confirming C6a without any new runtime code\n",
			    ratio);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
