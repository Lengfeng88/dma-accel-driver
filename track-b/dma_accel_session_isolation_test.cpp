// dma_accel_session_isolation_test.cpp
//
// M10.5 regression test for per-session buffer ownership.
//
// Before M10.5, filp->private_data pointed directly at the shared
// dma_accel_dev, so dev->buffers[] was a single global table with no
// notion of "whose slot is this". Two concrete bugs followed from that:
//   1. Any fd could SUBMIT (or mmap) against any other fd's buffer_id —
//      no ownership check existed anywhere on the ioctl/mmap path.
//   2. close()'ing ANY fd freed EVERY in-use buffer in the device,
//      including ones belonging to still-open fds (dma_accel_chr_release()
//      walked the whole buffers[] table, not "what this session
//      allocated").
//
// This test opens the device twice (fd_a, fd_b — two independent
// sessions, exactly like two unrelated client processes would each get
// by calling open() themselves) and checks three things:
//   (1) each session can alloc + submit + complete against its own
//       buffers — the ownership check doesn't break the legitimate
//       same-session case.
//   (2) fd_a submitting against fd_b's buffer_id (and vice versa) is
//       rejected with -EPERM, not silently allowed.
//   (3) closing fd_a does not disturb fd_b: a buffer fd_b allocated
//       before fd_a closed is still valid and submittable afterward.
//
// Deliberately standalone (no kctl), matching dma_accel_test.cpp's
// M9 diagnostic — this is a driver-level uAPI check, not something that
// benefits from kctl's concurrency primitives.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_session_isolation_test \
//       dma_accel_session_isolation_test.cpp
//
// Run (needs read/write on the device node, so likely sudo):
//   sudo ./dma_accel_session_isolation_test [/dev/dma_accel0]

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// See dma_accel_test.cpp for why this local typedef shim is needed —
// dma_accel_regs.h mixes kernel-internal (u32/u64) and uAPI (__u32/__u64)
// structs in one file without a __KERNEL__ split.
using u32 = std::uint32_t;
using u64 = std::uint64_t;
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace {

constexpr const char *kDefaultDevice = "/dev/dma_accel0";
constexpr std::uint32_t kBufSize = 4096;
constexpr int kPollTimeoutMs = 1000; // device sim latency is 50ms (spec §6)

int g_failures = 0;

void report(bool ok, const std::string &what) {
	if (ok) {
		std::printf("  PASS: %s\n", what.c_str());
	} else {
		std::printf("  FAIL: %s (errno=%d %s)\n", what.c_str(), errno, std::strerror(errno));
		++g_failures;
	}
}

// RAII guard for an mmap()'d region — same shape as dma_accel_test.cpp's.
class MappedBuffer {
public:
	MappedBuffer() = default;
	MappedBuffer(void *addr, std::size_t len) : addr_(addr), len_(len) {}
	MappedBuffer(const MappedBuffer &) = delete;
	MappedBuffer &operator=(const MappedBuffer &) = delete;
	MappedBuffer(MappedBuffer &&other) noexcept { *this = std::move(other); }
	MappedBuffer &operator=(MappedBuffer &&other) noexcept {
		reset();
		addr_ = other.addr_;
		len_ = other.len_;
		other.addr_ = nullptr;
		other.len_ = 0;
		return *this;
	}
	~MappedBuffer() { reset(); }

	void reset() {
		if (addr_ != nullptr && addr_ != MAP_FAILED) {
			munmap(addr_, len_);
		}
		addr_ = nullptr;
		len_ = 0;
	}

	std::uint8_t *data() const { return static_cast<std::uint8_t *>(addr_); }
	bool valid() const { return addr_ != nullptr && addr_ != MAP_FAILED; }

private:
	void *addr_ = nullptr;
	std::size_t len_ = 0;
};

// Allocates + maps a buffer on the given fd. Aborts the whole test on
// failure (an alloc failing here is an environment problem, not the
// thing under test) — mirrors dma_accel_test.cpp's alloc_and_map().
MappedBuffer alloc_and_map(int fd, std::uint32_t size, std::uint32_t *buffer_id) {
	dma_accel_buffer_alloc req{};
	req.size = size;

	if (ioctl(fd, DMA_ACCEL_IOC_BUFFER_ALLOC, &req) != 0) {
		std::fprintf(stderr, "FATAL: ioctl(BUFFER_ALLOC) failed: %s\n", std::strerror(errno));
		std::exit(1);
	}

	void *addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			   static_cast<off_t>(req.mmap_offset));
	if (addr == MAP_FAILED) {
		std::fprintf(stderr, "FATAL: mmap() failed: %s\n", std::strerror(errno));
		std::exit(1);
	}

	*buffer_id = req.buffer_id;
	return MappedBuffer(addr, size);
}

// Submits a COPY from src_id to dst_id on the given fd. Returns the
// ioctl's return value directly (0 on success, -1 with errno set on
// failure) instead of aborting — unlike alloc_and_map(), a failure here
// can be the expected outcome (the cross-session SUBMIT is *supposed*
// to fail), so the caller decides what a failure means.
int try_submit(int fd, std::uint32_t src_id, std::uint32_t dst_id, std::uint64_t *cmd_id_out) {
	dma_accel_submit submit{};
	submit.opcode = OPCODE_COPY;
	submit.len = kBufSize;
	submit.src_buffer_id = src_id;
	submit.src_offset = 0;
	submit.dst_buffer_id = dst_id;
	submit.dst_offset = 0;

	int ret = ioctl(fd, DMA_ACCEL_IOC_SUBMIT, &submit);
	if (ret == 0 && cmd_id_out != nullptr) {
		*cmd_id_out = submit.cmd_id;
	}
	return ret;
}

// Waits for and reads back one completion matching cmd_id. Aborts on
// timeout/error (a legitimate, same-session submit not completing is
// an environment/regression problem, not an expected test outcome).
void wait_for_completion(int fd, std::uint64_t cmd_id) {
	pollfd pfd{};
	pfd.fd = fd;
	pfd.events = POLLIN;

	int pret = poll(&pfd, 1, kPollTimeoutMs);
	if (pret <= 0 || (pfd.revents & POLLIN) == 0) {
		std::fprintf(stderr, "FATAL: poll() failed/timed out waiting for cmd_id=0x%llx\n",
			      static_cast<unsigned long long>(cmd_id));
		std::exit(1);
	}

	dma_accel_completion_uapi comp{};
	ssize_t n = read(fd, &comp, sizeof(comp));
	if (n != static_cast<ssize_t>(sizeof(comp))) {
		std::fprintf(stderr, "FATAL: read() returned %zd, expected %zu\n", n, sizeof(comp));
		std::exit(1);
	}
	if (comp.cmd_id != cmd_id || comp.status != DMA_ACCEL_OK) {
		std::fprintf(stderr,
			      "FATAL: unexpected completion cmd_id=0x%llx status=%u "
			      "(want cmd_id=0x%llx status=OK)\n",
			      static_cast<unsigned long long>(comp.cmd_id), comp.status,
			      static_cast<unsigned long long>(cmd_id));
		std::exit(1);
	}
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : kDefaultDevice;

	std::printf("== dma-accel M10.5 session-ownership isolation test ==\n");
	std::printf("opening %s twice (two independent sessions)\n", device);

	int fd_a = open(device, O_RDWR);
	if (fd_a < 0) {
		std::fprintf(stderr, "FATAL: open() for session A failed: %s\n", std::strerror(errno));
		return 1;
	}
	int fd_b = open(device, O_RDWR);
	if (fd_b < 0) {
		std::fprintf(stderr, "FATAL: open() for session B failed: %s\n", std::strerror(errno));
		close(fd_a);
		return 1;
	}

	// --- Setup: each session allocates its own src/dst pair --------------
	std::printf("\nallocating buffers for session A\n");
	std::uint32_t a_src_id = 0, a_dst_id = 0;
	MappedBuffer a_src = alloc_and_map(fd_a, kBufSize, &a_src_id);
	MappedBuffer a_dst = alloc_and_map(fd_a, kBufSize, &a_dst_id);
	std::memset(a_src.data(), 0xAA, kBufSize);
	std::memset(a_dst.data(), 0x00, kBufSize);
	std::printf("  session A: src_id=%u dst_id=%u\n", a_src_id, a_dst_id);

	std::printf("allocating buffers for session B\n");
	std::uint32_t b_src_id = 0, b_dst_id = 0;
	MappedBuffer b_src = alloc_and_map(fd_b, kBufSize, &b_src_id);
	MappedBuffer b_dst = alloc_and_map(fd_b, kBufSize, &b_dst_id);
	std::memset(b_src.data(), 0xBB, kBufSize);
	std::memset(b_dst.data(), 0x00, kBufSize);
	std::printf("  session B: src_id=%u dst_id=%u\n", b_src_id, b_dst_id);

	// --- (1) Same-session submit still works ------------------------------
	std::printf("\n[1] same-session submit (ownership check must not break the legit case)\n");
	{
		std::uint64_t cmd_id = 0;
		int ret = try_submit(fd_a, a_src_id, a_dst_id, &cmd_id);
		report(ret == 0, "A submit(A's src -> A's dst) succeeds");
		if (ret == 0) {
			wait_for_completion(fd_a, cmd_id);
			report(std::memcmp(a_src.data(), a_dst.data(), kBufSize) == 0,
			       "A's dst matches A's src after same-session COPY");
		}
	}
	{
		std::uint64_t cmd_id = 0;
		int ret = try_submit(fd_b, b_src_id, b_dst_id, &cmd_id);
		report(ret == 0, "B submit(B's src -> B's dst) succeeds");
		if (ret == 0) {
			wait_for_completion(fd_b, cmd_id);
			report(std::memcmp(b_src.data(), b_dst.data(), kBufSize) == 0,
			       "B's dst matches B's src after same-session COPY");
		}
	}

	// --- (2) Cross-session submit must be rejected -------------------------
	std::printf("\n[2] cross-session submit (must be denied, not silently allowed)\n");
	{
		int ret = try_submit(fd_a, b_src_id, a_dst_id, nullptr);
		report(ret != 0 && errno == EPERM,
		       "A submit(B's buffer_id as src) rejected with EPERM");
	}
	{
		int ret = try_submit(fd_a, a_src_id, b_dst_id, nullptr);
		report(ret != 0 && errno == EPERM,
		       "A submit(B's buffer_id as dst) rejected with EPERM");
	}
	{
		int ret = try_submit(fd_b, a_src_id, b_dst_id, nullptr);
		report(ret != 0 && errno == EPERM,
		       "B submit(A's buffer_id as src) rejected with EPERM");
	}

	// Also check mmap() directly refuses a foreign buffer_id — this is
	// the path dma_accel_ioc_submit()'s ownership check doesn't cover on
	// its own; ownership has to be enforced at mmap() too, or a session
	// could just skip SUBMIT and mmap the other session's memory
	// straight from userspace.
	std::printf("\n[2b] cross-session mmap (must also be denied)\n");
	{
		dma_accel_buffer_alloc fake_req{};
		fake_req.size = kBufSize;
		fake_req.buffer_id = b_src_id; // pretend we already know B's id
		fake_req.mmap_offset = static_cast<u64>(b_src_id) << 12; // PAGE_SHIFT
		void *addr = mmap(nullptr, kBufSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd_a,
				   static_cast<off_t>(fake_req.mmap_offset));
		bool denied = (addr == MAP_FAILED) && (errno == EPERM);
		report(denied, "A mmap(B's buffer_id) rejected with EPERM");
		if (addr != MAP_FAILED) {
			munmap(addr, kBufSize); // don't leak the mapping if the check regressed
		}
	}

	// --- (3) Closing one session must not disturb the other ---------------
	std::printf("\n[3] independent lifetime: close(A) must not touch B's resources\n");
	close(fd_a);
	a_src.reset(); // A's own mappings are now dangling anyway; drop them cleanly
	a_dst.reset();
	std::printf("  closed session A\n");

	{
		// B's dst buffer was already overwritten by step (1)'s COPY;
		// reset it and re-run the same COPY to prove B's buffers (and
		// the device state backing them) are still fully functional
		// after A's session tore down.
		std::memset(b_dst.data(), 0x00, kBufSize);
		std::uint64_t cmd_id = 0;
		int ret = try_submit(fd_b, b_src_id, b_dst_id, &cmd_id);
		report(ret == 0, "B submit still succeeds after A closed");
		if (ret == 0) {
			wait_for_completion(fd_b, cmd_id);
			report(std::memcmp(b_src.data(), b_dst.data(), kBufSize) == 0,
			       "B's dst still matches B's src after A closed");
		}
	}

	close(fd_b);

	std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
