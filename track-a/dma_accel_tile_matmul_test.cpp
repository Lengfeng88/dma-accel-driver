// dma_accel_tile_matmul_test.cpp
//
// First test of a real tile-level compute opcode: one 32x32 device-side
// matmul-accumulate, C_tile += A_tile @ B_tile, in a SINGLE submission —
// as opposed to the chained SCALE_ADD approach (dma_accel_gemm_row_via_scale_add.cpp),
// which needed K separate device round-trips to do comparable work. This
// is the piece that actually makes device compute viable at real GEMM
// scale: one TILE_MATMUL replaces DMA_ACCEL_TILE_DIM (32) chained
// SCALE_ADD calls' worth of accumulation.
//
// Starts the accumulator (C tile) at a nonzero, non-symmetric pattern
// (not zero) specifically to catch an "overwrite instead of accumulate"
// bug — a implementation that ignores the existing dst content would
// still pass a zero-initialized test.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_tile_matmul_test dma_accel_tile_matmul_test.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_tile_matmul_test [/dev/dma_accel0]

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
	constexpr std::uint32_t kDim = DMA_ACCEL_TILE_DIM; // 32

	std::printf("== dma-accel TILE_MATMUL test: C_tile += A_tile @ B_tile, %ux%u ==\n", kDim,
		    kDim);

	try {
		dma_accel::Stream stream(device);

		dma_accel::Buffer a = stream.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		dma_accel::Buffer b = stream.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		dma_accel::Buffer c = stream.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);

		auto *a_data = reinterpret_cast<float *>(a.data());
		auto *b_data = reinterpret_cast<float *>(b.data());
		auto *c_data = reinterpret_cast<float *>(c.data());

		std::vector<float> expected(kDim * kDim);
		for (std::uint32_t r = 0; r < kDim; ++r) {
			for (std::uint32_t col = 0; col < kDim; ++col) {
				a_data[r * kDim + col] = static_cast<float>((r * 3 + col) % 7);
				b_data[r * kDim + col] = static_cast<float>((r + col * 5) % 11);
				// Nonzero, non-symmetric starting accumulator — see
				// header comment for why this matters.
				c_data[r * kDim + col] = static_cast<float>((r == col) ? 100 : 0);
			}
		}
		for (std::uint32_t r = 0; r < kDim; ++r) {
			for (std::uint32_t col = 0; col < kDim; ++col) {
				float sum = c_data[r * kDim + col]; // start from the existing value
				for (std::uint32_t k = 0; k < kDim; ++k) {
					sum += a_data[r * kDim + k] * b_data[k * kDim + col];
				}
				expected[r * kDim + col] = sum;
			}
		}

		std::printf("submitting TILE_MATMUL (single device command does the full %ux%u "
			    "matmul-accumulate)\n",
			    kDim, kDim);
		dma_accel::Fence fence = stream.submit_tile_matmul(a, 0, b, 0, c, 0);
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
		for (std::uint32_t i = 0; i < kDim * kDim && ok; ++i) {
			if (c_data[i] != expected[i]) {
				std::fprintf(stderr,
					     "FAIL: element %u (row %u, col %u): device=%.2f "
					     "expected=%.2f\n",
					     i, i / kDim, i % kDim, c_data[i], expected[i]);
				ok = false;
			}
		}

		if (!ok) {
			return 1;
		}

		std::printf("PASS: single-command %ux%u device-side tile matmul-accumulate "
			    "correct — including verifying it ACCUMULATED onto the existing C "
			    "values rather than overwriting them\n",
			    kDim, kDim);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
