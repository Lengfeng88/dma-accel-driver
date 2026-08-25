// dma_accel_scheduler_test.cpp
//
// M13 regression test for Scheduler's round-robin dispatch.
//
// The property this milestone is actually about: with Context A having
// enqueued 3 requests and Context B having enqueued 3, a single
// run_round() call must dispatch them interleaved — A,B,A,B,A,B — not
// drain A's entire backlog before touching B's (A,A,A,B,B,B), which is
// what would happen if an application just called submit() directly in
// whatever order its own loops happened to write. This test enqueues a
// deliberately uneven mix (3 from A, 1 from B) specifically so the
// interleaved-vs-drained distinction is unambiguous in the observed
// order, and separately proves the underlying data actually moved
// correctly (not just that Fences came back in some order) and that
// M11's cross-Context provenance check still fires correctly even when
// dispatch is deferred through the Scheduler rather than called
// directly.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_scheduler_test \
//       dma_accel_scheduler_test.cpp dma_accel_scheduler.cpp \
//       dma_accel_context.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_scheduler_test [/dev/dma_accel0]

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "dma_accel_scheduler.hpp"

int g_failures = 0;

void report(bool ok, const std::string &what) {
	std::printf("  %s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
	if (!ok) {
		++g_failures;
	}
}

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";
	constexpr std::uint32_t kBufSize = 4096;

	std::printf("== dma-accel M13 Scheduler round-robin dispatch test ==\n\n");

	dma_accel::Scheduler sched;
	auto id_a = sched.add_context(dma_accel::Context("client-A", device));
	auto id_b = sched.add_context(dma_accel::Context("client-B", device));

	// A gets 3 src/dst pairs (3 independent COPY requests); B gets 1.
	// Deliberately uneven so the interleaved-vs-drained distinction is
	// unambiguous: if dispatch were "drain A then B," B's one request
	// would land at position 4 (last); round-robin puts it at position
	// 2 (right after A's first).
	dma_accel::Buffer a_src0 = sched.context(id_a).alloc_buffer(kBufSize);
	dma_accel::Buffer a_dst0 = sched.context(id_a).alloc_buffer(kBufSize);
	dma_accel::Buffer a_src1 = sched.context(id_a).alloc_buffer(kBufSize);
	dma_accel::Buffer a_dst1 = sched.context(id_a).alloc_buffer(kBufSize);
	dma_accel::Buffer a_src2 = sched.context(id_a).alloc_buffer(kBufSize);
	dma_accel::Buffer a_dst2 = sched.context(id_a).alloc_buffer(kBufSize);
	dma_accel::Buffer b_src0 = sched.context(id_b).alloc_buffer(kBufSize);
	dma_accel::Buffer b_dst0 = sched.context(id_b).alloc_buffer(kBufSize);

	std::memset(a_src0.data(), 0xA0, kBufSize);
	std::memset(a_src1.data(), 0xA1, kBufSize);
	std::memset(a_src2.data(), 0xA2, kBufSize);
	std::memset(b_src0.data(), 0xB0, kBufSize);

	sched.enqueue_copy(id_a, a_src0, 0, a_dst0, 0, kBufSize);
	sched.enqueue_copy(id_a, a_src1, 0, a_dst1, 0, kBufSize);
	sched.enqueue_copy(id_a, a_src2, 0, a_dst2, 0, kBufSize);
	sched.enqueue_copy(id_b, b_src0, 0, b_dst0, 0, kBufSize);

	std::printf("[1] enqueue order: A,A,A,B (3 from A, 1 from B) — dispatch must interleave\n");
	report(sched.has_pending(), "has_pending() is true before run_round()");

	auto dispatched = sched.run_round();
	report(!sched.has_pending(), "has_pending() is false after run_round() drains everything");
	report(dispatched.size() == 4, "run_round() dispatched exactly 4 requests");

	if (dispatched.size() == 4) {
		// Expected dispatch order: A, B, A, A — Context B's single
		// request rides along right after A's first, not after all
		// of A's three.
		bool order_ok = dispatched[0].first == id_a && dispatched[1].first == id_b &&
				 dispatched[2].first == id_a && dispatched[3].first == id_a;
		std::printf("  observed order: %s, %s, %s, %s\n",
			    dispatched[0].first == id_a ? "A" : "B",
			    dispatched[1].first == id_a ? "A" : "B",
			    dispatched[2].first == id_a ? "A" : "B",
			    dispatched[3].first == id_a ? "A" : "B");
		report(order_ok,
		       "dispatch order is A,B,A,A (round-robin) — not A,A,A,B (drain-then-drain)");
	}

	std::printf("\n[2] the data actually moved correctly for every dispatched request\n");
	bool all_waited_ok = true;
	for (auto &[ctx_id, fence] : dispatched) {
		if (!sched.context(ctx_id).wait(fence, 1000)) {
			all_waited_ok = false;
		}
	}
	report(all_waited_ok, "every dispatched Fence completes within timeout");
	report(std::memcmp(a_dst0.data(), a_src0.data(), kBufSize) == 0, "A's copy #0 landed correctly");
	report(std::memcmp(a_dst1.data(), a_src1.data(), kBufSize) == 0, "A's copy #1 landed correctly");
	report(std::memcmp(a_dst2.data(), a_src2.data(), kBufSize) == 0, "A's copy #2 landed correctly");
	report(std::memcmp(b_dst0.data(), b_src0.data(), kBufSize) == 0, "B's copy #0 landed correctly");

	std::printf("\n[3] M11's cross-Context check still fires when dispatch goes through Scheduler\n");
	{
		dma_accel::Buffer a_dst3 = sched.context(id_a).alloc_buffer(kBufSize);
		// Enqueue A submitting with B's buffer as src — the mistake
		// isn't caught at enqueue_copy() (which just stores
		// pointers), only at actual dispatch time inside run_round(),
		// same as it would be for a direct Context::submit() call.
		sched.enqueue_copy(id_a, b_src0, 0, a_dst3, 0, kBufSize);

		bool threw_logic_error = false;
		try {
			sched.run_round();
		} catch (const std::logic_error &) {
			threw_logic_error = true;
		} catch (const std::exception &e) {
			std::printf("  (caught WRONG exception type: %s)\n", e.what());
		}
		report(threw_logic_error,
		       "run_round() propagates std::logic_error for a cross-Context Buffer misuse");
	}

	std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
