// dma_accel_resource_quota_test.cpp
//
// M12 regression test for per-session buffer-slot quota.
//
// Before this fix, dev->buffers[MAX_BUFFERS=64] was a single pool with
// no per-session cap — a single greedy or buggy client could allocate
// every slot, starving every other concurrently open fd (nothing about
// M10.5's ownership fix prevented this; ownership answers "whose slot
// is this", not "how many slots is each client allowed"). This test
// proves the fix: each session is capped at DEFAULT_BUFFER_QUOTA
// (MAX_BUFFERS / DMA_ACCEL_MAX_INFLIGHT, see dma_accel_drv.c), and
// exhausting your own quota does not affect anyone else's.
//
// Checks, in order:
//   (1) A session can allocate exactly DEFAULT_BUFFER_QUOTA buffers.
//   (2) The next allocation past quota fails with EDQUOT specifically
//       (not ENOSPC, not some other error) — the caller can tell "I'm
//       over my own share" apart from "device is genuinely full."
//   (3) A second, independent session is completely unaffected: it can
//       still allocate its own full quota even while the first session
//       is sitting at its cap. This is the actual point of the
//       milestone — no single client can starve another.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_resource_quota_test \
//       dma_accel_resource_quota_test.cpp
// Run:
//   sudo ./dma_accel_resource_quota_test [/dev/dma_accel0]

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
// Must match dma_accel_drv.c's DEFAULT_BUFFER_QUOTA (MAX_BUFFERS / 4 =
// 64 / 4 = 16). Not derivable from the uAPI header at build time —
// there's no ioctl to query it (see M12 writeup: deliberately no
// SET_QUOTA/GET_QUOTA in v0), so this constant has to be kept in sync
// by hand with the driver source. If this test starts failing after a
// driver-side quota change, check this first before assuming a
// regression.
constexpr int kExpectedQuota = 16;

int g_failures = 0;

void report(bool ok, const std::string &what) {
	if (ok) {
		std::printf("  PASS: %s\n", what.c_str());
	} else {
		std::printf("  FAIL: %s (errno=%d %s)\n", what.c_str(), errno, std::strerror(errno));
		++g_failures;
	}
}

// Tries one BUFFER_ALLOC. Returns 0 and fills *buffer_id on success, or
// the negative errno on failure (mirroring what the ioctl itself
// reports via errno) — unlike alloc_and_map() in the other test files,
// failure here is frequently the expected outcome, so this doesn't
// abort the program.
int try_alloc(int fd, std::uint32_t *buffer_id) {
	dma_accel_buffer_alloc req{};
	req.size = kBufSize;
	if (ioctl(fd, DMA_ACCEL_IOC_BUFFER_ALLOC, &req) != 0) {
		return -errno;
	}
	*buffer_id = req.buffer_id;
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : kDefaultDevice;

	std::printf("== dma-accel M12 resource-quota regression test ==\n");
	std::printf("expected per-session quota: %d buffers\n\n", kExpectedQuota);

	int fd_a = open(device, O_RDWR);
	int fd_b = open(device, O_RDWR);
	if (fd_a < 0 || fd_b < 0) {
		std::fprintf(stderr, "FATAL: open() failed: %s\n", std::strerror(errno));
		return 1;
	}

	// --- (1) A can allocate exactly its quota -----------------------------
	std::printf("[1] session A allocates up to its quota\n");
	std::vector<std::uint32_t> a_ids;
	bool all_a_allocs_ok = true;
	for (int i = 0; i < kExpectedQuota; ++i) {
		std::uint32_t id;
		int ret = try_alloc(fd_a, &id);
		if (ret != 0) {
			all_a_allocs_ok = false;
			std::fprintf(stderr, "  alloc #%d unexpectedly failed: %s\n", i,
				     std::strerror(-ret));
			break;
		}
		a_ids.push_back(id);
	}
	report(all_a_allocs_ok && a_ids.size() == static_cast<size_t>(kExpectedQuota),
	       "A successfully allocates exactly its full quota");

	// --- (2) One more fails with EDQUOT, specifically ----------------------
	std::printf("\n[2] one more allocation past quota must fail with EDQUOT\n");
	{
		std::uint32_t id;
		int ret = try_alloc(fd_a, &id);
		report(ret == -EDQUOT,
		       "allocation past A's quota fails with EDQUOT (not ENOSPC or anything else)");
	}

	// --- (3) Session B is completely unaffected ----------------------------
	std::printf("\n[3] session B is unaffected by A sitting at its quota\n");
	std::vector<std::uint32_t> b_ids;
	bool all_b_allocs_ok = true;
	for (int i = 0; i < kExpectedQuota; ++i) {
		std::uint32_t id;
		int ret = try_alloc(fd_b, &id);
		if (ret != 0) {
			all_b_allocs_ok = false;
			std::fprintf(stderr, "  B's alloc #%d unexpectedly failed: %s\n", i,
				     std::strerror(-ret));
			break;
		}
		b_ids.push_back(id);
	}
	report(all_b_allocs_ok && b_ids.size() == static_cast<size_t>(kExpectedQuota),
	       "B allocates its own full quota while A is sitting at its cap — no starvation");

	// B should also independently hit its own EDQUOT past its quota,
	// same as A did — confirms the cap is per-session, not some shared
	// counter A and B are both drawing down.
	{
		std::uint32_t id;
		int ret = try_alloc(fd_b, &id);
		report(ret == -EDQUOT, "B independently hits its own EDQUOT past its own quota");
	}

	close(fd_a);
	close(fd_b);

	std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
