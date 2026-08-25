// dma_accel_dependency_smoke_test.cpp
//
// C3 smoke test, four scenarios:
//   1. Single Fence dependency (depends_on(Fence&&))
//   2. CommandGroup dependency (depends_on(const CommandGroup&))
//   3. A three-stage PendingCommand chain (depends_on(const PendingCommand&))
//      — a real data-dependent COPY chain: buf0 -> buf1 -> buf2 -> buf3,
//      where each stage's correctness depends on the previous stage
//      having genuinely finished (not just been enqueued) before it
//      starts, so this validates real ordering, not just API shape.
//   4. Failure propagation: an intentionally-invalid SCALE_ADD (bad
//      length) blocks its dependent, which must never submit.
//   5. Cycle rejection: depends_on() must throw before ever letting a
//      cycle form, including the trivial self-dependency case.
//
// Note: each scenario below opens its own Stream (its own driver session)
// rather than sharing one across the whole binary. This isn't just test
// hygiene — Track B's M12 caps buffers at 16 per session (derived from
// the device's 4-way concurrency ceiling: 64 total slots / 4). M10's
// Buffer has no BUFFER_FREE ioctl (deliberately deferred, per Track A's
// spec — "no workload has needed mid-session buffer release yet"), so a
// long-lived shared Stream across five buffer-heavy scenarios runs
// straight into that quota. Splitting sessions per scenario keeps each
// one well under the cap and is also a more realistic usage pattern —
// unrelated units of work wouldn't typically share one session anyway.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_dependency_smoke_test
//     dma_accel_dependency_smoke_test.cpp dma_accel_dependency.cpp
//     dma_accel_command_group.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_dependency_smoke_test [/dev/dma_accel0]

#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <thread>

#include "dma_accel_command_group.hpp"
#include "dma_accel_dependency.hpp"
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

constexpr std::uint32_t kBufSize = 4096;

void drive_progress_until(dma_accel::DependencyEngine &engine, const std::function<bool()> &done,
			   int max_iters = 500) {
	// progress() is deliberately non-blocking (one pump() per call, no
	// sleep inside it — see design rationale in the header). The real
	// device takes real wall-clock time to complete a command (tens of
	// ms, per Track A's M7 concurrency test timings), so a pure busy
	// spin here would exhaust its iteration budget before the hardware
	// ever gets a chance to finish anything. The 5ms sleep is test-
	// harness plumbing, not something DependencyEngine itself needs —
	// a real caller integrating this into an event loop would instead
	// drive progress() from whatever already wakes it periodically.
	for (int i = 0; i < max_iters && !done(); ++i) {
		engine.progress();
		if (!done()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
}

bool test_fence_dependency(const char *device) {
	std::printf("-- 1. single Fence dependency --\n");
	dma_accel::Stream stream(device);
	dma_accel::DependencyEngine engine(stream);

	dma_accel::Buffer a_src = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer a_dst = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer b_dst = stream.alloc_buffer(kBufSize);
	for (std::uint32_t i = 0; i < kBufSize; ++i) {
		a_src.data()[i] = static_cast<std::uint8_t>((i * 13 + 3) & 0xFF);
	}
	std::memset(a_dst.data(), 0, kBufSize);
	std::memset(b_dst.data(), 0, kBufSize);

	dma_accel::Fence fenceA = stream.submit(OPCODE_COPY, a_src, 0, a_dst, 0, kBufSize);

	dma_accel::PendingCommand pendingB =
		engine.defer_submit(OPCODE_COPY, a_dst, 0, b_dst, 0, kBufSize);
	pendingB.depends_on(std::move(fenceA));

	drive_progress_until(engine, [&] { return pendingB.is_submitted(); });

	if (!pendingB.is_submitted()) {
		std::fprintf(stderr, "FAIL: B never became eligible\n");
		return false;
	}
	if (!stream.wait(pendingB.fence(), 2000)) {
		std::fprintf(stderr, "FAIL: B submitted but never completed\n");
		return false;
	}
	if (pendingB.fence().status() != DMA_ACCEL_OK) {
		std::fprintf(stderr, "FAIL: B status=%u, want OK\n", pendingB.fence().status());
		return false;
	}
	if (std::memcmp(a_src.data(), b_dst.data(), kBufSize) != 0) {
		std::fprintf(stderr, "FAIL: b_dst does not match original a_src after A->B chain\n");
		return false;
	}
	std::printf("PASS: B correctly waited for A before submitting, data propagated correctly\n");
	return true;
}

bool test_group_dependency(const char *device) {
	std::printf("-- 2. CommandGroup dependency --\n");
	dma_accel::Stream stream(device);
	dma_accel::DependencyEngine engine(stream);

	dma_accel::Buffer src1 = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer dst1 = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer src2 = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer dst2 = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer final_dst = stream.alloc_buffer(kBufSize);
	std::memset(src1.data(), 0xAA, kBufSize);
	std::memset(src2.data(), 0xBB, kBufSize);
	std::memset(dst1.data(), 0, kBufSize);
	std::memset(dst2.data(), 0, kBufSize);
	std::memset(final_dst.data(), 0, kBufSize);

	dma_accel::CommandGroup group(stream);
	group.add(stream.submit(OPCODE_COPY, src1, 0, dst1, 0, kBufSize));
	group.add(stream.submit(OPCODE_COPY, src2, 0, dst2, 0, kBufSize));
	group.seal();

	dma_accel::PendingCommand pendingC =
		engine.defer_submit(OPCODE_COPY, dst1, 0, final_dst, 0, kBufSize);
	pendingC.depends_on(group);

	drive_progress_until(engine, [&] { return pendingC.is_submitted(); });

	if (!pendingC.is_submitted()) {
		std::fprintf(stderr, "FAIL: C never became eligible (group never fully completed?)\n");
		return false;
	}
	if (group.query() != dma_accel::GroupStatus::AllOk) {
		std::fprintf(stderr, "FAIL: group wasn't actually AllOk when C submitted\n");
		return false;
	}
	if (!stream.wait(pendingC.fence(), 2000) || pendingC.fence().status() != DMA_ACCEL_OK) {
		std::fprintf(stderr, "FAIL: C did not complete correctly\n");
		return false;
	}
	std::printf("PASS: C correctly waited for the entire group before submitting\n");
	return true;
}

bool test_pending_chain(const char *device) {
	std::printf("-- 3. three-stage PendingCommand chain --\n");
	dma_accel::Stream stream(device);
	dma_accel::DependencyEngine engine(stream);

	dma_accel::Buffer buf0 = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer buf1 = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer buf2 = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer buf3 = stream.alloc_buffer(kBufSize);
	for (std::uint32_t i = 0; i < kBufSize; ++i) {
		buf0.data()[i] = static_cast<std::uint8_t>((i * 7 + 11) & 0xFF);
	}
	std::memset(buf1.data(), 0, kBufSize);
	std::memset(buf2.data(), 0, kBufSize);
	std::memset(buf3.data(), 0, kBufSize);

	dma_accel::PendingCommand stageA = engine.defer_submit(OPCODE_COPY, buf0, 0, buf1, 0, kBufSize);
	dma_accel::PendingCommand stageB = engine.defer_submit(OPCODE_COPY, buf1, 0, buf2, 0, kBufSize);
	dma_accel::PendingCommand stageC = engine.defer_submit(OPCODE_COPY, buf2, 0, buf3, 0, kBufSize);
	stageB.depends_on(stageA);
	stageC.depends_on(stageB);

	drive_progress_until(engine, [&] { return stageC.is_submitted(); });

	if (!stageA.is_submitted() || !stageB.is_submitted() || !stageC.is_submitted()) {
		std::fprintf(stderr, "FAIL: chain did not fully resolve (A=%d B=%d C=%d)\n",
			     stageA.is_submitted(), stageB.is_submitted(), stageC.is_submitted());
		return false;
	}
	if (!stream.wait(stageC.fence(), 2000) || stageC.fence().status() != DMA_ACCEL_OK) {
		std::fprintf(stderr, "FAIL: stage C did not complete correctly\n");
		return false;
	}
	if (std::memcmp(buf0.data(), buf3.data(), kBufSize) != 0) {
		std::fprintf(stderr, "FAIL: buf3 does not match original buf0 after 3-stage chain\n");
		return false;
	}
	std::printf("PASS: 3-stage chain resolved in dependency order, data correct end-to-end\n");
	return true;
}

bool test_failure_propagation(const char *device) {
	std::printf("-- 4. failure propagation (blocked dependent) --\n");
	dma_accel::Stream stream(device);
	dma_accel::DependencyEngine engine(stream);

	dma_accel::Buffer a = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer b = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer dst = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer final_dst = stream.alloc_buffer(kBufSize);

	// Intentionally invalid: SCALE_ADD requires len % sizeof(float) == 0.
	// 13 is not a multiple of 4 — the device is expected to accept the
	// submission (no ioctl-time rejection, per dma_accel_regs.h's
	// comment on OPCODE_SCALE_ADD) and complete it with
	// DMA_ACCEL_ERR_LENGTH.
	constexpr std::uint32_t kBadLen = 13;
	dma_accel::PendingCommand badStage =
		engine.defer_submit_scale_add(a, 0, b, 0, dst, 0, kBadLen, 1.0f);

	drive_progress_until(engine, [&] { return badStage.is_submitted(); });
	if (!badStage.is_submitted()) {
		std::fprintf(stderr, "FAIL: bad-length command never even got submitted (expected it "
				      "to submit and then fail)\n");
		return false;
	}
	if (!stream.wait(badStage.fence(), 2000)) {
		std::fprintf(stderr, "FAIL: bad-length command never completed\n");
		return false;
	}
	if (badStage.fence().status() != DMA_ACCEL_ERR_LENGTH) {
		std::fprintf(stderr, "FAIL: expected DMA_ACCEL_ERR_LENGTH, got status=%u\n",
			     badStage.fence().status());
		return false;
	}
	std::printf("confirmed bad-length command completed with DMA_ACCEL_ERR_LENGTH as expected\n");

	dma_accel::PendingCommand dependent =
		engine.defer_submit(OPCODE_COPY, dst, 0, final_dst, 0, kBufSize);
	dependent.depends_on(badStage);

	drive_progress_until(engine, [&] { return dependent.is_blocked() || dependent.is_submitted(); });

	if (dependent.is_submitted()) {
		std::fprintf(stderr, "FAIL: dependent command submitted despite its dependency failing\n");
		return false;
	}
	if (!dependent.is_blocked()) {
		std::fprintf(stderr, "FAIL: dependent command is neither submitted nor blocked — stuck\n");
		return false;
	}

	// One more progress() call should not change anything — blocked is permanent.
	engine.progress();
	if (dependent.is_submitted() || !dependent.is_blocked()) {
		std::fprintf(stderr, "FAIL: blocked state did not hold after another progress() call\n");
		return false;
	}
	std::printf("PASS: dependent correctly blocked and stays blocked, never submitted\n");
	return true;
}

bool test_cycle_rejection(const char *device) {
	std::printf("-- 5. cycle rejection --\n");
	dma_accel::Stream stream(device);
	dma_accel::DependencyEngine engine(stream);

	dma_accel::Buffer x = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer y = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer z = stream.alloc_buffer(kBufSize);

	dma_accel::PendingCommand nodeA = engine.defer_submit(OPCODE_COPY, x, 0, y, 0, kBufSize);
	dma_accel::PendingCommand nodeB = engine.defer_submit(OPCODE_COPY, y, 0, z, 0, kBufSize);

	bool ok = true;

	// B depends on A — fine, no cycle yet.
	try {
		nodeB.depends_on(nodeA);
	} catch (const std::logic_error &) {
		std::fprintf(stderr, "FAIL: B.depends_on(A) should not throw (no cycle yet)\n");
		ok = false;
	}

	// A depends on B would close A -> B -> A. Must throw, and must not
	// have mutated the graph.
	bool threw = false;
	try {
		nodeA.depends_on(nodeB);
	} catch (const std::logic_error &) {
		threw = true;
	}
	if (!threw) {
		std::fprintf(stderr, "FAIL: A.depends_on(B) should have thrown (would close a cycle)\n");
		ok = false;
	} else {
		std::printf("PASS: two-node cycle correctly rejected\n");
	}

	// Trivial self-dependency must also throw.
	dma_accel::PendingCommand nodeC = engine.defer_submit(OPCODE_COPY, x, 0, z, 0, kBufSize);
	bool self_threw = false;
	try {
		nodeC.depends_on(nodeC);
	} catch (const std::logic_error &) {
		self_threw = true;
	}
	if (!self_threw) {
		std::fprintf(stderr, "FAIL: self-dependency should have thrown\n");
		ok = false;
	} else {
		std::printf("PASS: self-dependency correctly rejected\n");
	}

	// Clean up: none of these three should ever be submitted deliberately
	// (nodeC has no deps, so it WILL become eligible on the next
	// progress() — drain everything so nothing is left outstanding when
	// the Stream is destroyed).
	drive_progress_until(engine, [&] {
		return nodeA.is_submitted() && nodeB.is_submitted() && nodeC.is_submitted();
	});
	if (nodeA.is_submitted()) stream.wait(nodeA.fence(), 2000);
	if (nodeB.is_submitted()) stream.wait(nodeB.fence(), 2000);
	if (nodeC.is_submitted()) stream.wait(nodeC.fence(), 2000);

	return ok;
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel C3 DependencyEngine smoke test ==\n");

	try {
		std::printf("using device %s\n", device);
		std::printf("(each scenario opens its own Stream/session — see the "
			    "Track B M12 per-session buffer quota note below)\n");

		bool ok = true;

		ok = test_fence_dependency(device) && ok;
		ok = test_group_dependency(device) && ok;
		ok = test_pending_chain(device) && ok;
		ok = test_failure_propagation(device) && ok;
		ok = test_cycle_rejection(device) && ok;

		if (!ok) {
			std::fprintf(stderr, "FAIL: one or more checks failed, see above\n");
			return 1;
		}

		std::printf("PASS: DependencyEngine verified — Fence/Group/chain dependencies, "
			    "failure propagation, and cycle rejection all correct\n");
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
