// dma_accel_test.cpp
//
// M9 userspace ABI diagnostic tool for /dev/dma_accel0.
// Not a P2 component — a one-shot, single-threaded check that the
// ioctl(BUFFER_ALLOC) -> mmap -> ioctl(SUBMIT) -> poll -> read chain
// described in dma-accel-v0-register-spec.md §11 actually works end to
// end. Deliberately has zero dependencies (no kctl) — see rationale in
// the M9 design discussion: this tool's job is to validate the uAPI
// itself, not to exercise concurrency or batching, so kctl's lock-free
// ring buffers / thread pool would be unused weight, not real value.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_test dma_accel_test.cpp
//
// Run (needs read/write on the device node, so likely sudo):
//   sudo ./dma_accel_test [/dev/dma_accel0]

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

// dma_accel_regs.h declares two families of struct in one file:
//   - kernel-internal ones (dma_accel_cmd, dma_accel_completion), which
//     use the kernel's bare `u32`/`u64` typedefs (only ever meaningful
//     under __KERNEL__)
//   - the actual uAPI ones we want (dma_accel_buffer_alloc,
//     dma_accel_submit, dma_accel_completion_uapi), which correctly use
//     __u32/__u64 from <linux/types.h>
// The file was never split along that __KERNEL__ boundary, so it won't
// compile standalone in userspace as-is: `u32`/`u64` are undefined here.
// Rather than patch the frozen driver header, satisfy it locally — the
// kernel-internal structs it defines are otherwise unused in this file.
using u32 = std::uint32_t;
using u64 = std::uint64_t;
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace {

constexpr const char *kDefaultDevice = "/dev/dma_accel0";
constexpr std::uint32_t kBufSize = 4096; // == device per-command LEN cap (spec §2 note)
constexpr int kPollTimeoutMs = 1000; // device sim latency is 50ms (spec §6); generous margin

// RAII guard for an mmap()'d region — no kctl dependency, just enough
// to make early-return error paths below leak-free.
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

[[noreturn]] void fail(const std::string &what) {
	std::fprintf(stderr, "FAIL: %s: %s\n", what.c_str(), std::strerror(errno));
	std::exit(1);
}

// Allocates a device buffer and maps it. Returns the mmap'd region and
// fills buffer_id with the kernel-assigned handle.
MappedBuffer alloc_and_map(int fd, std::uint32_t size, std::uint32_t *buffer_id) {
	dma_accel_buffer_alloc req{};
	req.size = size;

	if (ioctl(fd, DMA_ACCEL_IOC_BUFFER_ALLOC, &req) != 0) {
		fail("ioctl(BUFFER_ALLOC)");
	}

	void *addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			   static_cast<off_t>(req.mmap_offset));
	if (addr == MAP_FAILED) {
		fail("mmap");
	}

	*buffer_id = req.buffer_id;
	std::printf("  allocated buffer_id=%u size=%u mmap_offset=0x%llx\n", req.buffer_id, size,
		    static_cast<unsigned long long>(req.mmap_offset));
	return MappedBuffer(addr, size);
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : kDefaultDevice;

	std::printf("== dma-accel M9 uAPI test ==\n");
	std::printf("opening %s\n", device);

	int fd = open(device, O_RDWR);
	if (fd < 0) {
		fail(std::string("open(") + device + ")");
	}

	std::printf("allocating src/dst buffers (%u bytes each)\n", kBufSize);
	std::uint32_t src_id = 0, dst_id = 0;
	MappedBuffer src = alloc_and_map(fd, kBufSize, &src_id);
	MappedBuffer dst = alloc_and_map(fd, kBufSize, &dst_id);

	// Deterministic, non-trivial test pattern — catches both
	// "copy didn't happen" (dst stays zero) and "copy is truncated /
	// off-by-one" (a repeating byte would hide those).
	for (std::uint32_t i = 0; i < kBufSize; ++i) {
		src.data()[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
	}
	std::memset(dst.data(), 0, kBufSize);

	std::printf("submitting COPY: src_buf=%u dst_buf=%u len=%u\n", src_id, dst_id, kBufSize);

	dma_accel_submit submit{};
	submit.opcode = OPCODE_COPY;
	submit.len = kBufSize;
	submit.src_buffer_id = src_id;
	submit.src_offset = 0;
	submit.dst_buffer_id = dst_id;
	submit.dst_offset = 0;
	// submit.cmd_id is an out param — the kernel allocates it (spec §11.1
	// decision 4, atomic64 counter), not us. DMA_ACCEL_IOC_SUBMIT is
	// _IOWR specifically so this comes back filled in below.

	if (ioctl(fd, DMA_ACCEL_IOC_SUBMIT, &submit) != 0) {
		fail("ioctl(SUBMIT)");
	}
	const std::uint64_t expected_cmd_id = submit.cmd_id;
	std::printf("  kernel assigned cmd_id=0x%llx\n",
		    static_cast<unsigned long long>(expected_cmd_id));

	std::printf("polling for completion (timeout %dms)\n", kPollTimeoutMs);
	pollfd pfd{};
	pfd.fd = fd;
	pfd.events = POLLIN;

	int pret = poll(&pfd, 1, kPollTimeoutMs);
	if (pret < 0) {
		fail("poll");
	}
	if (pret == 0) {
		std::fprintf(stderr, "FAIL: poll() timed out waiting for completion\n");
		return 1;
	}
	if ((pfd.revents & POLLIN) == 0) {
		std::fprintf(stderr, "FAIL: poll() returned without POLLIN (revents=0x%x)\n",
			      pfd.revents);
		return 1;
	}

	dma_accel_completion_uapi comp{};
	ssize_t n = read(fd, &comp, sizeof(comp));
	if (n < 0) {
		fail("read");
	}
	if (n != static_cast<ssize_t>(sizeof(comp))) {
		std::fprintf(stderr, "FAIL: read() returned %zd bytes, expected %zu\n", n,
			     sizeof(comp));
		return 1;
	}

	std::printf("completion: cmd_id=0x%llx status=%u\n",
		    static_cast<unsigned long long>(comp.cmd_id), comp.status);

	bool ok = true;

	if (comp.cmd_id != expected_cmd_id) {
		std::fprintf(stderr, "FAIL: cmd_id mismatch (got 0x%llx, want 0x%llx)\n",
			      static_cast<unsigned long long>(comp.cmd_id),
			      static_cast<unsigned long long>(expected_cmd_id));
		ok = false;
	}
	if (comp.status != DMA_ACCEL_OK) {
		std::fprintf(stderr, "FAIL: completion status=%u, want DMA_ACCEL_OK(0)\n", comp.status);
		ok = false;
	}
	if (std::memcmp(src.data(), dst.data(), kBufSize) != 0) {
		std::fprintf(stderr, "FAIL: dst buffer does not match src buffer after COPY\n");
		ok = false;
	}

	if (!ok) {
		return 1;
	}

	std::printf("PASS: ioctl/mmap/submit/poll/read chain verified, %u bytes copied correctly\n",
		    kBufSize);

	close(fd); // buffer lifetime is tied to the fd (spec §11.2) — this frees both buffers
	return 0;
}
