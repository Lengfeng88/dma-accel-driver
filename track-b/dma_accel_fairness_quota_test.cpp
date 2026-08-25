// dma_accel_fairness_quota_test.cpp
//
// M15 regression test for dynamic per-session fairness quota.
//
// Design recap (see M15-fairness.md): quota = ceil(DMA_ACCEL_QUEUE_DEPTH
// / active_sessions), recomputed at every submit — NOT a fixed number.
// With one session open this evaluates to 16 (the whole ring, identical
// to what Stream::throttle_before_submit() already assumed — M10's
// contract is untouched). Exhausting your own quota BLOCKS the
// ioctl(SUBMIT) call inside the kernel rather than returning an error —
// this is what lets Stream need zero code changes to cooperate with
// fairness it doesn't even know exists.
//
// Uses raw ioctl (not Stream) for the same reason M14's test did:
// Stream's own throttle would mask the kernel behavior this test is
// actually about. A background thread (std::async) is used specifically
// to observe "this call has not returned yet" — with a blocking-not-
// erroring design, that's the only way to actually confirm blocking
// happened rather than an immediate rejection.
//
// Checks:
//   (1) One session alone: quota is 16 (unaffected by this milestone),
//       flooding it with exactly 16 succeeds quickly — same behavior as
//       before M15, confirming the single-client case regressed nothing.
//   (2) Two sessions open: quota becomes 8 each. A can fill its own 8;
//       B can independently fill its own 8 (total = 16, exactly the
//       ring's physical capacity) — neither starves the other.
//   (3) A's 9th submit — past its own quota — does not return quickly
//       with an error; it's still pending (blocked) after a short wait,
//       confirmed via a background thread and a bounded poll.
//   (4) Once the device actually drains (sleep past simulated latency),
//       A's blocked 9th submit completes on its own — this was a
//       fairness wait, not a hang.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -pthread -o dma_accel_fairness_quota_test \
//       dma_accel_fairness_quota_test.cpp
// Run:
//   sudo ./dma_accel_fairness_quota_test [/dev/dma_accel0]

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
// Must match dma_accel_regs.h's DMA_ACCEL_QUEUE_DEPTH — see the same
// caveat as M12/M14's tests: no query ioctl exists, so this is kept in
// sync by hand with the driver source.
constexpr int kQueueDepth = 16;

int g_failures = 0;

void report(bool ok, const std::string &what) {
	if (ok) {
		std::printf("  PASS: %s\n", what.c_str());
	} else {
		std::printf("  FAIL: %s (errno=%d %s)\n", what.c_str(), errno, std::strerror(errno));
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

// Raw submit, deliberately not through Stream — see file comment.
// Returns 0 on success, -errno on failure. This call may BLOCK inside
// the kernel per M15's design — callers that want to observe blocking
// must invoke this from a background thread (see check [3] below).
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

	std::printf("== dma-accel M15 fairness-quota regression test ==\n\n");

	// --- (1) One session alone: quota is 16, unaffected by M15 -----------
	std::printf("[1] one session alone — quota must be the full ring (16), unaffected by M15\n");
	{
		int fd = open(device, O_RDWR);
		if (fd < 0) {
			std::fprintf(stderr, "FATAL: open() failed: %s\n", std::strerror(errno));
			return 1;
		}
		std::uint32_t src = alloc_buffer(fd);
		std::uint32_t dst = alloc_buffer(fd);

		auto start = std::chrono::steady_clock::now();
		bool all_16_ok = true;
		for (int i = 0; i < kQueueDepth; ++i) {
			if (raw_submit(fd, src, dst) != 0) {
				all_16_ok = false;
				break;
			}
		}
		auto elapsed = std::chrono::steady_clock::now() - start;
		auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

		report(all_16_ok, "all 16 back-to-back submits succeed with one session open");
		// Generous bound — this should take well under a millisecond in
		// practice (16 syscalls, no blocking expected); 30ms is chosen
		// to be comfortably below the ~50ms simulated device latency, so
		// "fast" here specifically rules out "quietly blocked and then
		// timed out/proceeded for an unrelated reason."
		report(elapsed_ms < 30,
		       "those 16 submits completed quickly (no unexpected blocking for a lone session)");

		close(fd);
		usleep(150 * 1000); // let the device fully drain before test [2]
	}

	// --- (2) Two sessions: quota becomes 8 each ---------------------------
	std::printf("\n[2] two sessions open — quota becomes 8 each, neither starves the other\n");
	int fd_a = open(device, O_RDWR);
	int fd_b = open(device, O_RDWR);
	if (fd_a < 0 || fd_b < 0) {
		std::fprintf(stderr, "FATAL: open() failed: %s\n", std::strerror(errno));
		return 1;
	}
	std::uint32_t a_src = alloc_buffer(fd_a);
	std::uint32_t a_dst = alloc_buffer(fd_a);
	std::uint32_t b_src = alloc_buffer(fd_b);
	std::uint32_t b_dst = alloc_buffer(fd_b);

	constexpr int kExpectedQuota = kQueueDepth / 2; // 8, with exactly 2 sessions open
	bool a_ok = true, b_ok = true;
	for (int i = 0; i < kExpectedQuota; ++i) {
		if (raw_submit(fd_a, a_src, a_dst) != 0) {
			a_ok = false;
		}
	}
	for (int i = 0; i < kExpectedQuota; ++i) {
		if (raw_submit(fd_b, b_src, b_dst) != 0) {
			b_ok = false;
		}
	}
	report(a_ok, "A fills its own quota (8) successfully");
	report(b_ok, "B independently fills its own quota (8) successfully, unaffected by A");

	// --- (3) A's 9th (past its own quota) blocks, doesn't error ----------
	std::printf("\n[3] A's 9th submit (past its own quota) must BLOCK, not return an error\n");
	auto blocked_submit = std::async(std::launch::async, [&]() { return raw_submit(fd_a, a_src, a_dst); });

	// Poll for a bounded, short duration — well under the ~50ms
	// simulated device latency, so if the call has already returned by
	// now, it did NOT block (it either errored immediately or, if this
	// bound is too tight for the test machine, would be a false
	// failure — 40ms leaves real margin below 50ms).
	auto status = blocked_submit.wait_for(std::chrono::milliseconds(40));
	bool still_pending = status == std::future_status::timeout;
	report(still_pending,
	       "A's 9th submit is still pending after 40ms — genuinely blocked, not an immediate error");
	if (!still_pending) {
		// Diagnostic: the call already returned. Print its ACTUAL
		// result (not main thread's possibly-stale global errno,
		// which report() would otherwise print and which has
		// nothing to do with this background call at all) so a
		// failure here is actually debuggable.
		int early_result = blocked_submit.get();
		std::printf("  (diagnostic: the call returned early with result=%d [%s])\n",
			    early_result, std::strerror(-early_result));
		std::printf("\n%s: %d failure(s)\n", "FAIL", ++g_failures);
		close(fd_a);
		close(fd_b);
		return 1;
	}

	// --- (4) Once the ring drains, the blocked call completes on its own -
	std::printf("\n[4] once the device drains, A's blocked submit completes by itself\n");
	int blocked_result = blocked_submit.get(); // blocks until it actually finishes
	if (blocked_result == 0) {
		std::printf("  PASS: A's previously-blocked 9th submit eventually succeeds\n");
	} else {
		std::printf("  FAIL: A's previously-blocked 9th submit returned %d [%s]\n", blocked_result,
			    std::strerror(-blocked_result));
		++g_failures;
	}

	close(fd_a);
	close(fd_b);

	std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
