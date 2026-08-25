// dma_accel_runtime_smoke_test.cpp
//
// M10 smoke test: the same scenario dma_accel_test.cpp (M9) validated via
// raw ioctl/mmap/poll/read, now through the Stream/Buffer/Fence API. If
// this passes, M10's abstraction is a correct lift of the M9 ABI, not
// just a plausible-looking one.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_runtime_smoke_test dma_accel_runtime_smoke_test.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_runtime_smoke_test [/dev/dma_accel0]

#include <cstdio>
#include <cstring>
#include <exception>

#include "dma_accel_runtime.hpp"

// Only for OPCODE_COPY / DMA_ACCEL_OK — same shim dma_accel_runtime.cpp
// uses, see there for why it's needed.
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
} // namespace
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";
	constexpr std::uint32_t kBufSize = 4096;

	std::printf("== dma-accel M10 runtime smoke test ==\n");

	try {
		dma_accel::Stream stream(device);
		std::printf("opened %s\n", device);

		dma_accel::Buffer src = stream.alloc_buffer(kBufSize);
		dma_accel::Buffer dst = stream.alloc_buffer(kBufSize);
		std::printf("allocated buffer_id=%u and buffer_id=%u (%u bytes each)\n",
			    src.buffer_id(), dst.buffer_id(), kBufSize);

		for (std::uint32_t i = 0; i < kBufSize; ++i) {
			src.data()[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
		}
		std::memset(dst.data(), 0, kBufSize);

		dma_accel::Fence fence =
			stream.submit(OPCODE_COPY, src, 0, dst, 0, kBufSize);
		std::printf("submitted, kernel-assigned cmd_id=0x%llx\n",
			    static_cast<unsigned long long>(fence.cmd_id()));

		bool done = stream.wait(fence, 1000);
		if (!done) {
			std::fprintf(stderr, "FAIL: wait() timed out\n");
			return 1;
		}

		std::printf("completion: status=%u\n", fence.status());

		bool ok = true;
		if (fence.status() != DMA_ACCEL_OK) {
			std::fprintf(stderr, "FAIL: status=%u, want DMA_ACCEL_OK(0)\n", fence.status());
			ok = false;
		}
		if (std::memcmp(src.data(), dst.data(), kBufSize) != 0) {
			std::fprintf(stderr, "FAIL: dst does not match src after COPY\n");
			ok = false;
		}
		if (!ok) {
			return 1;
		}

		std::printf("PASS: Stream/Buffer/Fence API verified, %u bytes copied correctly\n",
			    kBufSize);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
