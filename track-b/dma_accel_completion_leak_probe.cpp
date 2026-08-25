// dma_accel_completion_leak_probe.cpp
//
// M10.5 known-gap probe (NOT a pass/fail regression test — see
// M10.5-session-ownership.md, "Known gap"). dev->comp_ring is still a
// single completion queue shared by every open() session, consumed via
// one global comp_ring_head index (dma_accel_drv.c). This program
// exists to show *exactly* what that means in practice, rather than
// leave it as a theoretical concern:
//
//   B submits a command.
//   A calls read() before B does.
//   -> does A's read() return B's completion (cmd_id/status leak)?
//   -> does B's own subsequent read()/poll() then see nothing at all
//      (the entry was already consumed out from under it)?
//
// Buffer ownership (M10.5's actual fix) does NOT protect against this —
// that check lives in SUBMIT/mmap, not in read(). This probe exercises
// read()/poll() only; it never touches a foreign buffer_id.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_completion_leak_probe \
//       dma_accel_completion_leak_probe.cpp
// Run:
//   sudo ./dma_accel_completion_leak_probe [/dev/dma_accel0]

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

using u32 = std::uint32_t;
using u64 = std::uint64_t;
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace {

constexpr const char *kDefaultDevice = "/dev/dma_accel0";
constexpr std::uint32_t kBufSize = 4096;
constexpr int kPollTimeoutMs = 1000;

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

// Tries to read one completion within timeout_ms. Returns true and
// fills *out if one arrived, false on timeout (not an error here —
// "nothing arrived" is itself a meaningful probe result).
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

	std::printf("== M10.5 known-gap probe: shared completion ring ==\n");
	std::printf("(this is diagnostic, not pass/fail — see M10.5-session-ownership.md)\n\n");

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

	std::printf("B submits its own COPY (A submits nothing yet)\n");
	std::uint64_t b_cmd_id = submit_copy(fd_b, b_src, b_dst);
	std::printf("  B's cmd_id = 0x%llx\n\n", static_cast<unsigned long long>(b_cmd_id));

	// Give the device time to actually finish (sim latency ~50ms per
	// spec §6) so the completion is sitting in comp_ring before A reads,
	// isolating "who drains it" from "did it even finish yet".
	usleep(150 * 1000);

	std::printf("A calls read() first (A submitted nothing — should have nothing of its own)\n");
	dma_accel_completion_uapi a_comp{};
	bool a_got_something = try_read_one(fd_a, kPollTimeoutMs, &a_comp);

	if (!a_got_something) {
		std::printf("  A's read() got nothing (timed out).\n");
		std::printf("  => comp_ring appears to be per-fd after all, OR B's completion\n");
		std::printf("     hadn't posted yet. Re-check with a longer sleep before concluding\n");
		std::printf("     the gap doesn't reproduce.\n\n");
	} else {
		std::printf("  A's read() returned: cmd_id=0x%llx status=%u\n",
			    static_cast<unsigned long long>(a_comp.cmd_id), a_comp.status);
		if (a_comp.cmd_id == b_cmd_id) {
			std::printf("  => CONFIRMED: A observed and consumed B's completion.\n"
				    "     comp_ring_head is a single global index — A's read() call\n"
				    "     advanced it, so this entry is now gone for B too.\n\n");
		} else {
			std::printf("  => A got a completion, but not B's cmd_id (0x%llx) — unexpected,\n"
				    "     investigate further before drawing a conclusion.\n\n",
				    static_cast<unsigned long long>(b_cmd_id));
		}
	}

	std::printf("Now B tries to read() its own completion\n");
	dma_accel_completion_uapi b_comp{};
	bool b_got_something = try_read_one(fd_b, kPollTimeoutMs, &b_comp);

	if (!b_got_something) {
		std::printf("  B's read() got nothing (timed out).\n");
		if (a_got_something && a_comp.cmd_id == b_cmd_id) {
			std::printf("  => CONFIRMED: B's own completion was silently lost — consumed by\n"
				    "     A's earlier read(), not delivered to B at all. This is not just\n"
				    "     an information leak, it's a correctness hazard: B's Fence would\n"
				    "     never become ready and Stream::wait() would hang until timeout.\n");
		} else {
			std::printf("  => B lost its completion for a different reason — investigate.\n");
		}
	} else {
		std::printf("  B's read() returned: cmd_id=0x%llx status=%u\n",
			    static_cast<unsigned long long>(b_comp.cmd_id), b_comp.status);
		if (b_comp.cmd_id == b_cmd_id) {
			std::printf("  => B got its own completion after all. Combined with A's result\n"
				    "     above, decide whether this run demonstrates the gap or not —\n"
				    "     timing-sensitive; consider rerunning a few times.\n");
		}
	}

	close(fd_a);
	close(fd_b);
	std::printf("\ndone. This probe intentionally always exits 0 — it reports what it\n"
		    "observed, it doesn't assert what should have happened.\n");
	return 0;
}
