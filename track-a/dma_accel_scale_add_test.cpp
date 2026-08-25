// dma_accel_scale_add_test.cpp
//
// First test of a real compute opcode: out[i] = a[i]*scalar + b[i],
// computed by the DEVICE (dma_accel_exec_scale_add() in the QEMU model),
// not by the CPU. Everything in M11 stages 1-3 was CPU-side math over
// device-allocated memory; this is the first time the accelerator
// actually accelerates something.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_scale_add_test dma_accel_scale_add_test.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_scale_add_test [/dev/dma_accel0]

#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

#include "dma_accel_runtime.hpp"

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
	constexpr std::uint32_t kN = 1024; // elements
	constexpr std::uint32_t kBufSize = kN * sizeof(float);
	constexpr float kScalar = 2.5f;

	std::printf("== dma-accel SCALE_ADD test: out[i] = a[i]*%.2f + b[i], %u elements ==\n",
		    kScalar, kN);

	try {
		dma_accel::Stream stream(device);

		dma_accel::Buffer a = stream.alloc_buffer(kBufSize);
		dma_accel::Buffer b = stream.alloc_buffer(kBufSize);
		dma_accel::Buffer out = stream.alloc_buffer(kBufSize);

		auto *a_data = reinterpret_cast<float *>(a.data());
		auto *b_data = reinterpret_cast<float *>(b.data());
		auto *out_data = reinterpret_cast<float *>(out.data());

		std::vector<float> expected(kN);
		for (std::uint32_t i = 0; i < kN; ++i) {
			a_data[i] = static_cast<float>(i) - 500.0f;
			b_data[i] = static_cast<float>(i * 2 % 17);
			expected[i] = a_data[i] * kScalar + b_data[i];
		}
		std::memset(out_data, 0, kBufSize);

		std::printf("submitting SCALE_ADD (device computes, not CPU)\n");
		dma_accel::Fence fence = stream.submit_scale_add(a, 0, b, 0, out, 0, kBufSize, kScalar);
		std::printf("kernel-assigned cmd_id=0x%llx\n",
			    static_cast<unsigned long long>(fence.cmd_id()));

		if (!stream.wait(fence, 1000)) {
			std::fprintf(stderr, "FAIL: wait() timed out\n");
			return 1;
		}
		std::printf("completion status=%u\n", fence.status());

		bool ok = true;
		if (fence.status() != DMA_ACCEL_OK) {
			std::fprintf(stderr, "FAIL: status=%u, want DMA_ACCEL_OK(0)\n", fence.status());
			ok = false;
		}
		std::uint32_t first_bad = 0;
		for (std::uint32_t i = 0; i < kN && ok; ++i) {
			if (out_data[i] != expected[i]) {
				std::fprintf(stderr,
					     "FAIL: element %u: device=%.3f expected=%.3f "
					     "(a=%.3f scalar=%.2f b=%.3f)\n",
					     i, out_data[i], expected[i], a_data[i], kScalar, b_data[i]);
				ok = false;
				first_bad = i;
				(void)first_bad;
			}
		}

		if (!ok) {
			return 1;
		}

		std::printf("PASS: all %u elements computed correctly BY THE DEVICE — first real "
			    "accelerator compute in this project, not CPU math over DMA memory\n",
			    kN);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
