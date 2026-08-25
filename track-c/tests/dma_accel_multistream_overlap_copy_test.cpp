// dma_accel_multistream_overlap_copy_test.cpp
//
// C6a diagnostic variant: identical methodology to
// dma_accel_multistream_overlap_test.cpp, but using OPCODE_COPY instead
// of OPCODE_TILE_MATMUL.
//
// Why this variant exists: the TILE_MATMUL version measured a ratio of
// T_concurrent/T_single close to 1.8-2.0 (near-serial) even after ruling
// out session-count/quota confounds. Track A's own history is a clue:
// the M7 self-test that empirically demonstrated 4-way concurrent
// execution ("MAX_INFLIGHT=4 on device") predates M11's addition of the
// TILE_MATMUL/SCALE_ADD compute opcodes — meaning genuine hardware
// overlap was only ever validated for the basic COPY/DMA path. It's
// entirely possible TILE_MATMUL is handled synchronously/compute-bound
// in the device model in a way COPY is not. This variant isolates that
// by testing the exact same overlap methodology against the one opcode
// Track A already proved overlaps.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_multistream_overlap_copy_test
//     dma_accel_multistream_overlap_copy_test.cpp dma_accel_command_group.cpp
//     dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_multistream_overlap_copy_test [/dev/dma_accel0]

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
constexpr std::uint32_t kBufSize = 4096;

std::pair<std::chrono::milliseconds, bool> run_copies(dma_accel::Stream &stream, int count) {
	dma_accel::Buffer src = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer dst = stream.alloc_buffer(kBufSize);
	std::memset(src.data(), 0x11, kBufSize);
	std::memset(dst.data(), 0, kBufSize);

	dma_accel::CommandGroup group(stream);

	auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < count; ++i) {
		group.add(stream.submit(OPCODE_COPY, src, 0, dst, 0, kBufSize));
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

	std::printf("== dma-accel C6a overlap test (OPCODE_COPY variant) ==\n");
	std::printf("using device %s\n", device);

	try {
		std::chrono::milliseconds t_single{};
		{
			std::printf("-- baseline: single Stream, %d COPY commands --\n", kCommandsPerStream);
			dma_accel::Stream baseline_stream(device);
			auto [duration, ok_single] = run_copies(baseline_stream, kCommandsPerStream);
			if (!ok_single) {
				std::fprintf(stderr, "FAIL: baseline commands did not all complete OK\n");
				return 1;
			}
			t_single = duration;
			std::printf("T_single = %lldms\n", static_cast<long long>(t_single.count()));
		}

		std::printf("-- concurrent: two independent Streams, %d COPY commands each --\n",
			    kCommandsPerStream);
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

		dma_accel::CommandGroup groupA(streamA);
		dma_accel::CommandGroup groupB(streamB);

		auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < kCommandsPerStream; ++i) {
			groupA.add(streamA.submit(OPCODE_COPY, srcA, 0, dstA, 0, kBufSize));
		}
		for (int i = 0; i < kCommandsPerStream; ++i) {
			groupB.add(streamB.submit(OPCODE_COPY, srcB, 0, dstB, 0, kBufSize));
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
				     "still >= 1.5 even with OPCODE_COPY (ratio %.2f) — if TILE_MATMUL's "
				     "ratio was similar, this rules out 'wrong opcode' as the explanation; "
				     "something else is serializing cross-session execution\n",
				     ratio);
			return 1;
		}

		std::printf("PASS: ratio %.2f < 1.5 with OPCODE_COPY — if this is meaningfully lower than "
			    "the TILE_MATMUL run, that confirms TILE_MATMUL specifically doesn't overlap "
			    "in this device model, while basic COPY does, matching Track A's M7 history\n",
			    ratio);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
