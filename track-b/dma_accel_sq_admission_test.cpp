// dma_accel_sq_admission_test.cpp
//
// M14 regression test for kernel-side SQ ring admission control.
// UPDATED BY M15 (twice):
//   1. M15 superseded M14's original immediate-EBUSY behavior with
//      blocking admission (see M15-fairness.md) — checks [2]/[3] below
//      assert eventual success after a real blocking delay, not an
//      error return.
//   2. fd_b is now opened LATE (right before check [3]), not at the
//      start. M15's per-session quota is ceil(16 / active_sessions),
//      and active_sessions counts every OPEN fd regardless of whether
//      it has submitted anything — an earlier version of this file
//      opened fd_b at the very start (unused during checks [1]-[2]) and
//      that alone silently cut A's own quota to 8 for the entire test,
//      invalidating checks [1]-[2]'s "single session, full 16-deep
//      ring" premise even though every individual assertion still
//      happened to report PASS (blocking got amortized invisibly
//      across the flood loop instead of being caught explicitly). Kept
//      here as documented history since it's a real lesson: a
//      dependency between two fds' behavior can be silent until you
//      specifically look for whether one is open, not whether it did
//      anything.
//
// This test's scope after that correction is deliberately narrower than
// it once tried to be: it proves the PHYSICAL ring's hard cap (M14) in
// as close to isolation from M15's per-session fairness math as
// possible — checks [1]-[2] run with exactly one session open (quota =
// full ring = 16, so any blocking observed there is unambiguously the
// M14 physical gate, not M15 fairness). Check [3] only confirms a
// second session can submit normally once it exists; it deliberately
// does NOT re-derive multi-session quota arithmetic — that is
// dma_accel_fairness_quota_test.cpp's job, and duplicating it here with
// slightly different setup would just create two tests that could
// disagree about the same numbers for no good reason.
//
// Before the M14 fix itself, dma_accel_queue_submit_ext() wrote into
// sq_buf[] (physically DMA_ACCEL_QUEUE_DEPTH=16 slots) with zero
// capacity check. The only admission check was against cmd_table's
// capacity (64, generously sized as bookkeeping headroom), which could
// never trigger anywhere near where the real 16-slot ring actually
// filled. The only thing that had ever kept callers under 16 was
// Stream's userspace throttle — a courtesy, not a kernel guarantee.
//
// This test therefore deliberately does NOT use dma_accel_runtime.hpp
// (Stream) at all — using Stream would just re-exercise its throttle
// and never reach the kernel path this fix is actually about. It calls
// ioctl(DMA_ACCEL_IOC_SUBMIT) directly.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -pthread -o dma_accel_sq_admission_test \
//       dma_accel_sq_admission_test.cpp
// Run:
//   sudo ./dma_accel_sq_admission_test [/dev/dma_accel0]

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

using u32 = std::uint32_t;
using u64 = std::uint64_t;
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace {

constexpr const char *kDefaultDevice = "/dev/dma_accel0";
constexpr std::uint32_t kBufSize = 4096;
constexpr int kQueueDepth = 16;

int g_failures = 0;

void report(bool ok, const std::string &what) {
	if (ok) {
		std::printf("  PASS: %s\n", what.c_str());
	} else {
		std::printf("  FAIL: %s\n", what.c_str());
		++g_failures;
	}
}

std::uint32_t alloc_buffer(int fd) {
	dma_accel_buffer_alloc req{};
	req.size = kBufSize;
	if (ioctl(fd, DMA_ACCEL_IOC_BUFFER_ALLOC, &req) != 0) {
		std::fprintf(stderr, "FATAL: BUFFER_ALLOC: %s\n", std::strerror(errno));
		std::exit(1);
	}
	return req.buffer_id;
}

// Raw submit, deliberately NOT going through Stream — see file comment.
// Returns 0 on success, -errno on failure. May BLOCK inside the kernel.
int raw_submit(int fd, std::uint32_t src_id, std::uint32_t dst_id) {
	dma_accel_submit submit{};
	submit.opcode = OPCODE_COPY;
	submit.len = kBufSize;
	submit.src_buffer_id = src_id;
	submit.dst_buffer_id = dst_id;
	if (ioctl(fd, DMA_ACCEL_IOC_SUBMIT, &submit) != 0) {
		return -errno;
	}
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : kDefaultDevice;

	std::printf("== dma-accel M14/M15 SQ-ring admission control test ==\n");
	std::printf("(deliberately bypasses Stream's userspace throttle — raw ioctl only)\n\n");

	// Only fd_a is open for checks [1]-[2] — see file comment on why
	// fd_b's open() is deferred until right before check [3].
	int fd_a = open(device, O_RDWR);
	if (fd_a < 0) {
		std::fprintf(stderr, "FATAL: open() failed: %s\n", std::strerror(errno));
		return 1;
	}
	std::uint32_t a_src = alloc_buffer(fd_a);
	std::uint32_t a_dst = alloc_buffer(fd_a);

	// --- (1) One session alone floods the full physical ring -------------
	std::printf("[1] one session alone: %d back-to-back raw submits (no throttle)\n", kQueueDepth);
	{
		auto start = std::chrono::steady_clock::now();
		bool all_16_ok = true;
		for (int i = 0; i < kQueueDepth; ++i) {
			int ret = raw_submit(fd_a, a_src, a_dst);
			if (ret != 0) {
				all_16_ok = false;
				std::fprintf(stderr, "  submit #%d unexpectedly failed: %s\n", i,
					     std::strerror(-ret));
				break;
			}
		}
		auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
					   std::chrono::steady_clock::now() - start)
					   .count();
		report(all_16_ok, "all 16 back-to-back submits (the ring's real capacity) succeed");
		// With exactly one session open, M15's quota is the full ring
		// (16) — this bound catches a regression to the earlier
		// version of this file's mistake (fd_b silently open,
		// quietly capping A at 8 and amortizing blocking invisibly
		// across this same loop instead of failing loudly).
		report(elapsed_ms < 30, "those 16 submits were genuinely fast — no hidden per-session blocking");
	}

	// --- (2) The 17th (physical ring cap, single session) blocks then succeeds -
	std::printf("\n[2] the 17th submit — physical ring cap, still only one session — must block then succeed\n");
	{
		auto call = std::async(std::launch::async, [&]() { return raw_submit(fd_a, a_src, a_dst); });
		bool still_pending =
			call.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout;
		report(still_pending, "17th submit is still pending after 30ms — blocked, not an immediate error");

		int result = call.get();
		report(result == 0, "17th submit eventually succeeds once the ring drains");
	}

	usleep(150 * 1000); // let A fully drain before introducing a second session

	// --- (3) A second session opens and works normally --------------------
	std::printf("\n[3] a second session opens and can submit normally\n");
	int fd_b = open(device, O_RDWR);
	if (fd_b < 0) {
		std::fprintf(stderr, "FATAL: open() for B failed: %s\n", std::strerror(errno));
		close(fd_a);
		return 1;
	}
	std::uint32_t b_src = alloc_buffer(fd_b);
	std::uint32_t b_dst = alloc_buffer(fd_b);
	{
		int ret = raw_submit(fd_b, b_src, b_dst);
		report(ret == 0, "B's first submit succeeds immediately (device is idle, plenty of quota)");
	}
	{
		int ret = raw_submit(fd_a, a_src, a_dst);
		report(ret == 0, "A can also still submit normally now that B has opened too");
	}
	std::printf("  (multi-session quota arithmetic itself is covered by\n"
		    "   dma_accel_fairness_quota_test.cpp — not re-tested here)\n");

	close(fd_a);
	close(fd_b);

	std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
