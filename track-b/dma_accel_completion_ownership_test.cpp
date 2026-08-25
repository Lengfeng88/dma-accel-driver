// dma_accel_completion_ownership_test.cpp
//
// M10.5 regression test for per-session completion routing.
//
// Before this fix, dev->comp_ring was a single completion queue shared
// by every open() session, drained via one global comp_ring_head index.
// Confirmed experimentally (dma_accel_completion_leak_probe.cpp): any
// session's read() could consume ANY other session's completion —
// not just observe it, but steal it, since comp_ring_head advanced
// regardless of which fd called read(). The victim session's Fence
// would then never become ready; Stream::wait() would hang until
// timeout with no error to explain why.
//
// The fix adds a kernel-side cmd_id -> session routing table
// (cmd_table) consulted by the ISR, plus a completion ring/waitqueue
// per session instead of one shared globally. This test proves the
// three properties that fix is supposed to guarantee:
//
//   (1) A session that submitted nothing sees nothing, even while
//       another session's completion is sitting there ready.
//   (2) Two sessions that both submit each get exactly their own
//       completion — no cross-contamination, no loss.
//   (3) Which session calls read() first doesn't matter: routing is
//       determined by which session *submitted* the command, not by
//       read() call order across fds.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_completion_ownership_test \
//       dma_accel_completion_ownership_test.cpp
// Run:
//   sudo ./dma_accel_completion_ownership_test [/dev/dma_accel0]

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
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
// Device sim latency is ~50ms (spec §6). Generous margin for "should
// complete by now" waits; short margin for "should NOT see anything"
// checks uses a separate, smaller constant below.
constexpr int kPollTimeoutMs = 1000;
// Used only where the expected outcome is "poll() finds nothing" — long
// enough to not be a flaky false-negative against normal completion
// latency, short enough not to make a failing run (where something
// unexpected DOES arrive) slow to observe.
constexpr int kNoEventTimeoutMs = 300;

int g_failures = 0;

void report(bool ok, const std::string &what) {
	if (ok) {
		std::printf("  PASS: %s\n", what.c_str());
	} else {
		std::printf("  FAIL: %s (errno=%d %s)\n", what.c_str(), errno, std::strerror(errno));
		++g_failures;
	}
}

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

private:
	void *addr_ = nullptr;
	std::size_t len_ = 0;
};

MappedBuffer alloc_and_map(int fd, std::uint32_t size, std::uint32_t *buffer_id) {
	dma_accel_buffer_alloc req{};
	req.size = size;
	if (ioctl(fd, DMA_ACCEL_IOC_BUFFER_ALLOC, &req) != 0) {
		std::fprintf(stderr, "FATAL: BUFFER_ALLOC: %s\n", std::strerror(errno));
		std::exit(1);
	}
	void *addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			   static_cast<off_t>(req.mmap_offset));
	if (addr == MAP_FAILED) {
		std::fprintf(stderr, "FATAL: mmap: %s\n", std::strerror(errno));
		std::exit(1);
	}
	*buffer_id = req.buffer_id;
	return MappedBuffer(addr, size);
}

// Submits a same-session COPY. Aborts on failure — every submit in this
// file is a legitimate same-session one (this test is about completion
// routing, not buffer ownership, which dma_accel_session_isolation_test
// already covers), so a submit failing here is an environment problem.
std::uint64_t submit_copy(int fd, std::uint32_t src_id, std::uint32_t dst_id) {
	dma_accel_submit submit{};
	submit.opcode = OPCODE_COPY;
	submit.len = kBufSize;
	submit.src_buffer_id = src_id;
	submit.dst_buffer_id = dst_id;
	if (ioctl(fd, DMA_ACCEL_IOC_SUBMIT, &submit) != 0) {
		std::fprintf(stderr, "FATAL: SUBMIT: %s\n", std::strerror(errno));
		std::exit(1);
	}
	return submit.cmd_id;
}

// Polls fd for up to timeout_ms and reads one completion if available.
// Returns true + fills *out on success, false on timeout/EAGAIN — a
// clean false is a meaningful result in this file (it's what "this
// session correctly sees nothing" looks like), not treated as fatal.
bool try_read_one(int fd, int timeout_ms, dma_accel_completion_uapi *out) {
	pollfd pfd{};
	pfd.fd = fd;
	pfd.events = POLLIN;
	int pret = poll(&pfd, 1, timeout_ms);
	if (pret <= 0) {
		return false;
	}
	ssize_t n = read(fd, out, sizeof(*out));
	return n == static_cast<ssize_t>(sizeof(*out));
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : kDefaultDevice;

	std::printf("== dma-accel M10.5 completion-ownership regression test ==\n");
	std::printf("opening %s twice (two independent sessions)\n\n", device);

	int fd_a = open(device, O_RDWR);
	int fd_b = open(device, O_RDWR);
	if (fd_a < 0 || fd_b < 0) {
		std::fprintf(stderr, "FATAL: open() failed: %s\n", std::strerror(errno));
		return 1;
	}

	std::uint32_t a_src, a_dst, b_src, b_dst;
	MappedBuffer a_src_buf = alloc_and_map(fd_a, kBufSize, &a_src);
	MappedBuffer a_dst_buf = alloc_and_map(fd_a, kBufSize, &a_dst);
	MappedBuffer b_src_buf = alloc_and_map(fd_b, kBufSize, &b_src);
	MappedBuffer b_dst_buf = alloc_and_map(fd_b, kBufSize, &b_dst);
	std::memset(a_src_buf.data(), 0xAA, kBufSize);
	std::memset(b_src_buf.data(), 0xBB, kBufSize);

	// --- (1) Only B submits: A must see nothing, B must see its own ------
	std::printf("[1] only B submits — A must observe nothing, B must get its own\n");
	{
		std::uint64_t b_cmd = submit_copy(fd_b, b_src, b_dst);
		std::printf("  B submitted cmd_id=0x%llx\n", static_cast<unsigned long long>(b_cmd));

		dma_accel_completion_uapi comp{};
		bool a_saw_something = try_read_one(fd_a, kNoEventTimeoutMs, &comp);
		report(!a_saw_something, "A sees nothing while only B has a pending/ready completion");

		bool b_ok = try_read_one(fd_b, kPollTimeoutMs, &comp);
		report(b_ok && comp.cmd_id == b_cmd && comp.status == DMA_ACCEL_OK,
		       "B receives its own completion, correct cmd_id and status");
	}

	// --- (2) Both submit: each gets exactly its own -----------------------
	std::printf("\n[2] both A and B submit — each must get exactly its own\n");
	{
		std::uint64_t a_cmd = submit_copy(fd_a, a_src, a_dst);
		std::uint64_t b_cmd = submit_copy(fd_b, b_src, b_dst);
		std::printf("  A submitted cmd_id=0x%llx, B submitted cmd_id=0x%llx\n",
			    static_cast<unsigned long long>(a_cmd), static_cast<unsigned long long>(b_cmd));

		dma_accel_completion_uapi a_comp{}, b_comp{};
		bool a_ok = try_read_one(fd_a, kPollTimeoutMs, &a_comp);
		bool b_ok = try_read_one(fd_b, kPollTimeoutMs, &b_comp);

		report(a_ok && a_comp.cmd_id == a_cmd, "A's read() returns A's own cmd_id, not B's");
		report(b_ok && b_comp.cmd_id == b_cmd, "B's read() returns B's own cmd_id, not A's");

		// Neither session should have anything left after consuming
		// its one completion.
		dma_accel_completion_uapi leftover{};
		report(!try_read_one(fd_a, kNoEventTimeoutMs, &leftover),
		       "A has nothing left after reading its one completion");
		report(!try_read_one(fd_b, kNoEventTimeoutMs, &leftover),
		       "B has nothing left after reading its one completion");
	}

	// --- (3) Read-order independence: B reads first even though A --------
	//         submitted first. Routing must follow who *submitted* the
	//         command, not who calls read() first or submission order.
	std::printf("\n[3] read-order independence — B reads first despite A submitting first\n");
	{
		std::uint64_t a_cmd = submit_copy(fd_a, a_src, a_dst);
		std::uint64_t b_cmd = submit_copy(fd_b, b_src, b_dst);
		std::printf("  submission order: A (0x%llx) then B (0x%llx)\n",
			    static_cast<unsigned long long>(a_cmd), static_cast<unsigned long long>(b_cmd));

		// Let both actually finish on the device before either side
		// reads, so this isn't just re-testing "whoever reads first
		// wins" — both completions are already sitting in their
		// respective per-session rings by the time we read either.
		usleep(150 * 1000);

		dma_accel_completion_uapi b_comp{}, a_comp{};
		bool b_ok = try_read_one(fd_b, kPollTimeoutMs, &b_comp); // B reads first
		bool a_ok = try_read_one(fd_a, kPollTimeoutMs, &a_comp); // then A

		report(b_ok && b_comp.cmd_id == b_cmd,
		       "B (reading first) still gets B's own cmd_id, not A's earlier one");
		report(a_ok && a_comp.cmd_id == a_cmd,
		       "A (reading second) still gets A's own cmd_id, unaffected by B reading first");
	}

	// --- (4) Multiple outstanding on one session, one on the other -------
	std::printf("\n[4] A has several outstanding, B has one — no cross-contamination\n");
	{
		constexpr int kN = 3;
		std::vector<std::uint64_t> a_cmds;
		for (int i = 0; i < kN; ++i) {
			a_cmds.push_back(submit_copy(fd_a, a_src, a_dst));
		}
		std::uint64_t b_cmd = submit_copy(fd_b, b_src, b_dst);
		usleep(200 * 1000); // let all of them finish

		int a_seen = 0;
		bool a_all_match = true;
		for (int i = 0; i < kN; ++i) {
			dma_accel_completion_uapi comp{};
			if (!try_read_one(fd_a, kPollTimeoutMs, &comp)) {
				break;
			}
			++a_seen;
			bool matches_one_of_a =
				std::find(a_cmds.begin(), a_cmds.end(), comp.cmd_id) != a_cmds.end();
			if (!matches_one_of_a) {
				a_all_match = false;
			}
		}
		report(a_seen == kN, "A receives exactly its own N=3 completions, not more or fewer");
		report(a_all_match, "every completion A received matches one of A's own cmd_ids");

		dma_accel_completion_uapi b_comp{};
		bool b_ok = try_read_one(fd_b, kPollTimeoutMs, &b_comp);
		report(b_ok && b_comp.cmd_id == b_cmd,
		       "B still gets exactly its own single completion, unaffected by A's batch");
	}

	close(fd_a);
	close(fd_b);

	std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
