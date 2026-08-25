// dma_accel_command_group_smoke_test.cpp
//
// C1 smoke test: submits several independent COPY commands on one
// Stream, tracks them as a single CommandGroup, and verifies both the
// happy path (seal -> wait -> AllOk, correct data in every buffer) and
// the two lifecycle guards (add() after seal(), query()/wait() before
// seal() both throw std::logic_error).
//
// This deliberately reuses the same COPY-correctness check M10's smoke
// test used (dma_accel_runtime_smoke_test.cpp) — C1 isn't testing new
// device behavior, it's testing that grouping/aggregation is correct on
// top of already-verified single-command execution.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_command_group_smoke_test
//     dma_accel_command_group_smoke_test.cpp dma_accel_command_group.cpp
//     dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_command_group_smoke_test [/dev/dma_accel0]

#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <vector>

#include "dma_accel_command_group.hpp"
#include "dma_accel_runtime.hpp"

// Same shim as the other test/runtime files — see dma_accel_runtime.cpp
// for the fuller explanation.
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
} // namespace
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace {

constexpr std::uint32_t kBufSize = 4096;
constexpr int kGroupSize = 4;

bool run_happy_path(dma_accel::Stream &stream) {
	std::printf("-- happy path: %d commands in one CommandGroup --\n", kGroupSize);

	std::vector<dma_accel::Buffer> srcs;
	std::vector<dma_accel::Buffer> dsts;
	srcs.reserve(kGroupSize);
	dsts.reserve(kGroupSize);

	for (int i = 0; i < kGroupSize; ++i) {
		srcs.push_back(stream.alloc_buffer(kBufSize));
		dsts.push_back(stream.alloc_buffer(kBufSize));
		// Distinct fill pattern per pair so a mixed-up copy (e.g. group
		// bookkeeping accidentally comparing the wrong src/dst) would
		// show up as a mismatch instead of accidentally matching.
		for (std::uint32_t b = 0; b < kBufSize; ++b) {
			srcs[i].data()[b] = static_cast<std::uint8_t>((b * 31 + 7 + i * 17) & 0xFF);
		}
		std::memset(dsts[i].data(), 0, kBufSize);
	}

	dma_accel::CommandGroup group(stream);
	for (int i = 0; i < kGroupSize; ++i) {
		group.add(stream.submit(OPCODE_COPY, srcs[i], 0, dsts[i], 0, kBufSize));
	}
	std::printf("added %zu commands to group\n", group.size());

	group.seal();
	dma_accel::GroupStatus status = group.wait(2000);

	if (status != dma_accel::GroupStatus::AllOk) {
		std::fprintf(stderr, "FAIL: group.wait() returned %s, want AllOk\n",
			     status == dma_accel::GroupStatus::Pending ? "Pending (timeout)" : "SomeFailed");
		return false;
	}

	bool ok = true;
	for (int i = 0; i < kGroupSize; ++i) {
		if (std::memcmp(srcs[i].data(), dsts[i].data(), kBufSize) != 0) {
			std::fprintf(stderr, "FAIL: pair %d does not match after COPY\n", i);
			ok = false;
		}
	}
	if (!ok) {
		return false;
	}

	std::printf("PASS: all %d commands completed AllOk, all data verified correct\n", kGroupSize);
	return true;
}

bool run_lifecycle_guards(dma_accel::Stream &stream) {
	std::printf("-- lifecycle guards --\n");
	bool ok = true;

	// query()/wait() before seal() must throw.
	{
		dma_accel::CommandGroup group(stream);
		bool threw = false;
		try {
			group.query();
		} catch (const std::logic_error &) {
			threw = true;
		}
		if (!threw) {
			std::fprintf(stderr, "FAIL: query() before seal() did not throw\n");
			ok = false;
		} else {
			std::printf("PASS: query() before seal() threw std::logic_error\n");
		}
	}

	// add() after seal() must throw, and must not disturb the fence the
	// caller tried to add (it's a rejected add, not a partial one).
	{
		dma_accel::Buffer src = stream.alloc_buffer(kBufSize);
		dma_accel::Buffer dst = stream.alloc_buffer(kBufSize);
		std::memset(src.data(), 0xAB, kBufSize);

		dma_accel::CommandGroup group(stream);
		group.seal(); // seal an empty group immediately

		dma_accel::Fence extra = stream.submit(OPCODE_COPY, src, 0, dst, 0, kBufSize);
		bool threw = false;
		try {
			group.add(std::move(extra));
		} catch (const std::logic_error &) {
			threw = true;
		}
		if (!threw) {
			std::fprintf(stderr, "FAIL: add() after seal() did not throw\n");
			ok = false;
		} else {
			std::printf("PASS: add() after seal() threw std::logic_error\n");
		}

		// The throw happened before fences_.push_back() ran inside
		// add(), so `extra` in this scope is still the original,
		// unmoved Fence — drain it explicitly so nothing outstanding
		// is left dangling when the Stream is destroyed.
		if (!stream.wait(extra, 2000)) {
			std::fprintf(stderr, "FAIL: draining the rejected extra command timed out\n");
			ok = false;
		}
	}

	// Empty, sealed group: vacuously AllOk, not an error and not Pending.
	{
		dma_accel::CommandGroup group(stream);
		group.seal();
		dma_accel::GroupStatus status = group.query();
		if (status != dma_accel::GroupStatus::AllOk) {
			std::fprintf(stderr, "FAIL: empty sealed group query() != AllOk\n");
			ok = false;
		} else {
			std::printf("PASS: empty sealed group is vacuously AllOk\n");
		}
	}

	return ok;
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel C1 CommandGroup smoke test ==\n");

	try {
		dma_accel::Stream stream(device);
		std::printf("opened %s\n", device);

		bool ok = run_happy_path(stream);
		ok = run_lifecycle_guards(stream) && ok;

		if (!ok) {
			std::fprintf(stderr, "FAIL: one or more checks failed, see above\n");
			return 1;
		}

		std::printf("PASS: CommandGroup verified — aggregation, sealing, and lifecycle guards all correct\n");
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
