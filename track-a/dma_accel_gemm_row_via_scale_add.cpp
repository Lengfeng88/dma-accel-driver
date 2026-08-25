// dma_accel_gemm_row_via_scale_add.cpp
//
// Minimal correctness proof that SCALE_ADD can actually compute a piece
// of GEMM, not just an isolated a*scalar+b in a vacuum. One row of
// C = A @ B: C[:] = sum_{k=0}^{K-1} A_row[k] * B[k][:], computed as a
// CHAIN of K device-side SCALE_ADD calls, each depending on the previous
// one's output:
//
//   acc[0] = B[0][:] * A_row[0] + zero_vector
//   acc[k] = B[k][:] * A_row[k] + acc[k-1]      for k = 1..K-1
//   C[:]   = acc[K-1]
//
// This is deliberately NOT an attempt to redo the full GEMM stage 1-3
// workloads on the device — at that granularity (one SCALE_ADD per
// scalar-times-row term) stage 3's shape alone would be on the order of
// ~290,000 submissions, most of them serially dependent (can't overlap,
// since each step needs the previous step's completion), against a
// device with a fixed ~50ms simulated latency per command. That's not a
// correctness question, just an unrealistic amount of wall-clock time
// for what this is trying to prove. This program proves the chaining
// itself is correct at a small, tractable scale (K=8 steps); scaling
// SCALE_ADD up to cover a full GEMM is a separate question (almost
// certainly wanting a wider/batched compute opcode, not K-deep chains of
// this one) that isn't answered by this test one way or the other.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_gemm_row_via_scale_add dma_accel_gemm_row_via_scale_add.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_gemm_row_via_scale_add [/dev/dma_accel0]

#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

#include "dma_accel_runtime.hpp"

namespace {

constexpr std::uint32_t kN = 1024; // row length (elements)
constexpr std::uint32_t kK = 8;    // number of terms summed — small on purpose, see header
constexpr std::uint32_t kBufSize = kN * sizeof(float);

float *as_floats(dma_accel::Buffer &buf) { return reinterpret_cast<float *>(buf.data()); }

float a_row_value(std::uint32_t k) { return static_cast<float>(k + 1) * 0.5f; }
float b_value(std::uint32_t k, std::uint32_t i) {
	return static_cast<float>((i % 13) + k);
}

std::vector<float> reference_row() {
	std::vector<float> c(kN, 0.0f);
	for (std::uint32_t k = 0; k < kK; ++k) {
		const float a = a_row_value(k);
		for (std::uint32_t i = 0; i < kN; ++i) {
			c[i] += a * b_value(k, i);
		}
	}
	return c;
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel: one GEMM row via chained SCALE_ADD ==\n");
	std::printf("C[:] = sum_{k=0}^{%u} A_row[k] * B[k][:], row length %u, %u chained "
		    "device-side steps\n",
		    kK - 1, kN, kK);

	try {
		dma_accel::Stream stream(device);

		// B[0..K-1]: K row buffers.
		std::vector<dma_accel::Buffer> b_rows;
		b_rows.reserve(kK);
		for (std::uint32_t k = 0; k < kK; ++k) {
			b_rows.push_back(stream.alloc_buffer(kBufSize));
			float *dst = as_floats(b_rows.back());
			for (std::uint32_t i = 0; i < kN; ++i) {
				dst[i] = b_value(k, i);
			}
		}

		// Zero vector — stands in for acc[-1] on the first step.
		dma_accel::Buffer zero = stream.alloc_buffer(kBufSize);
		std::memset(zero.data(), 0, kBufSize);

		// One accumulator buffer per step, so each intermediate result
		// is individually inspectable if something goes wrong (not
		// trying to save buffer count here — K=8 means 8 accumulators,
		// nowhere near MAX_BUFFERS=64).
		std::vector<dma_accel::Buffer> acc;
		acc.reserve(kK);

		std::printf("submitting chain (each step waits for the previous one's "
			    "completion — this is a genuine dependency chain, not a batch)\n");
		for (std::uint32_t k = 0; k < kK; ++k) {
			acc.push_back(stream.alloc_buffer(kBufSize));
			const dma_accel::Buffer &prev = (k == 0) ? zero : acc[k - 1];
			const float scalar = a_row_value(k);

			dma_accel::Fence fence = stream.submit_scale_add(
				b_rows[k], 0, prev, 0, acc[k], 0, kBufSize, scalar);
			if (!stream.wait(fence, 1000)) {
				std::fprintf(stderr, "FAIL: step %u timed out\n", k);
				return 1;
			}
			if (fence.status() != 0) {
				std::fprintf(stderr, "FAIL: step %u status=%u\n", k, fence.status());
				return 1;
			}
			std::printf("  step %u: acc[%u] = B[%u][:] * %.1f + %s — cmd_id=0x%llx OK\n", k,
				    k, k, scalar, k == 0 ? "zero_vector" : "acc[k-1]",
				    static_cast<unsigned long long>(fence.cmd_id()));
		}

		std::printf("computing reference row on host for comparison\n");
		std::vector<float> reference = reference_row();

		float *result = as_floats(acc.back());
		bool ok = true;
		for (std::uint32_t i = 0; i < kN; ++i) {
			if (result[i] != reference[i]) {
				std::fprintf(stderr,
					     "FAIL: element %u: device-chained=%.4f reference=%.4f\n",
					     i, result[i], reference[i]);
				ok = false;
				break;
			}
		}

		if (!ok) {
			return 1;
		}

		std::printf("PASS: all %u elements of the GEMM row match the reference — %u "
			    "chained device-side SCALE_ADD calls correctly compute a real "
			    "matrix-multiply row, not just an isolated a*scalar+b\n",
			    kN, kK);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
