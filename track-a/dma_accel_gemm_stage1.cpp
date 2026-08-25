// dma_accel_gemm_stage1.cpp
//
// M11 phase 1: does the tiled-GEMM indexing/math work, using Stream's
// buffers as plain RAII-managed memory? Deliberately does NOT call
// submit()/wait() — Stream's queue/backpressure machinery was already
// proven correct (and stress-tested past DMA_ACCEL_QUEUE_DEPTH) in M10.
// This file's only new variable is: is the tiling correct. Mixing in a
// COPY access pattern here would test two unrelated things at once.
//
// Shape (deliberately avoids two complexities not yet exercised):
//   A is M x K, B is K x N, C is M x N, all tiled at TILE x TILE.
//   M = N = 256, K = 128 (== TILE) so:
//     - no remainder tiles (M, N, K all divide evenly by TILE)
//     - no K-direction accumulation (K is exactly one tile, so each
//       C tile depends on exactly one (A tile, B tile) pair)
//   That's 2x1 A tiles, 1x2 B tiles, 2x2 C tiles = 8 buffers total,
//   comfortably under MAX_BUFFERS (64).
//
// Build (use -O2 — this is a real, if small, triple-nested-loop matmul):
//   g++ -std=c++17 -O2 -Wall -Wextra -o dma_accel_gemm_stage1 dma_accel_gemm_stage1.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_gemm_stage1 [/dev/dma_accel0]

#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

#include "dma_accel_runtime.hpp"

namespace {

constexpr std::uint32_t kTile = 128;
constexpr std::uint32_t kM = 256;
constexpr std::uint32_t kK = 128; // == kTile: single K-tile, no accumulation needed
constexpr std::uint32_t kN = 256;

constexpr std::uint32_t kTileBytes = kTile * kTile * sizeof(float);
constexpr std::uint32_t kMTiles = kM / kTile; // 2
constexpr std::uint32_t kNTiles = kN / kTile; // 2
// kKTiles is always 1 here (kK == kTile) — not a separate constant since
// there is deliberately no accumulation loop over it in phase 1.

float *as_floats(dma_accel::Buffer &buf) { return reinterpret_cast<float *>(buf.data()); }

// Deterministic, non-symmetric fill so a transposed-index bug (a very
// easy mistake in tiled GEMM) shows up as a mismatch instead of
// accidentally cancelling out.
float a_value(std::uint32_t global_row, std::uint32_t global_col) {
	return static_cast<float>((global_row * 3 + global_col) % 7);
}
float b_value(std::uint32_t global_row, std::uint32_t global_col) {
	return static_cast<float>((global_row + global_col * 5) % 11);
}

// Reference implementation, operating on plain host arrays with no tiling
// at all — the ground truth the tiled version is checked against.
std::vector<float> reference_gemm() {
	std::vector<float> a(kM * kK), b(kK * kN), c(kM * kN, 0.0f);
	for (std::uint32_t r = 0; r < kM; ++r) {
		for (std::uint32_t k = 0; k < kK; ++k) {
			a[r * kK + k] = a_value(r, k);
		}
	}
	for (std::uint32_t k = 0; k < kK; ++k) {
		for (std::uint32_t c = 0; c < kN; ++c) {
			b[k * kN + c] = b_value(k, c);
		}
	}
	for (std::uint32_t r = 0; r < kM; ++r) {
		for (std::uint32_t k = 0; k < kK; ++k) {
			const float a_rk = a[r * kK + k];
			for (std::uint32_t col = 0; col < kN; ++col) {
				c[r * kN + col] += a_rk * b[k * kN + col];
			}
		}
	}
	return c;
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel M11 stage 1: tiled GEMM indexing check ==\n");
	std::printf("A: %ux%u, B: %ux%u, C: %ux%u, tile: %ux%u (%u A-tiles, %u B-tiles, %u C-tiles)\n",
		    kM, kK, kK, kN, kM, kN, kTile, kTile, kMTiles, kNTiles, kMTiles * kNTiles);

	try {
		dma_accel::Stream stream(device);

		// A tiles: kMTiles row-blocks, each TILE x K (K == TILE here).
		std::vector<dma_accel::Buffer> a_tiles;
		for (std::uint32_t ti = 0; ti < kMTiles; ++ti) {
			a_tiles.push_back(stream.alloc_buffer(kTileBytes));
			float *dst = as_floats(a_tiles.back());
			for (std::uint32_t r = 0; r < kTile; ++r) {
				const std::uint32_t global_row = ti * kTile + r;
				for (std::uint32_t k = 0; k < kTile; ++k) {
					dst[r * kTile + k] = a_value(global_row, k);
				}
			}
		}

		// B tiles: kNTiles col-blocks, each K x TILE (K == TILE here).
		std::vector<dma_accel::Buffer> b_tiles;
		for (std::uint32_t tj = 0; tj < kNTiles; ++tj) {
			b_tiles.push_back(stream.alloc_buffer(kTileBytes));
			float *dst = as_floats(b_tiles.back());
			for (std::uint32_t k = 0; k < kTile; ++k) {
				for (std::uint32_t c = 0; c < kTile; ++c) {
					const std::uint32_t global_col = tj * kTile + c;
					dst[k * kTile + c] = b_value(k, global_col);
				}
			}
		}

		std::printf("computing %u C tiles (%ux%u each) on CPU, reading directly from A/B "
			    "tile Buffers\n",
			    kMTiles * kNTiles, kTile, kTile);

		// C tiles: kMTiles x kNTiles grid. Each depends on exactly one
		// (A tile, B tile) pair since kK == kTile (no accumulation).
		std::vector<dma_accel::Buffer> c_tiles;
		c_tiles.reserve(kMTiles * kNTiles);
		for (std::uint32_t ti = 0; ti < kMTiles; ++ti) {
			for (std::uint32_t tj = 0; tj < kNTiles; ++tj) {
				c_tiles.push_back(stream.alloc_buffer(kTileBytes));
				const float *a_data = as_floats(a_tiles[ti]);
				const float *b_data = as_floats(b_tiles[tj]);
				float *c_data = as_floats(c_tiles.back());
				std::memset(c_data, 0, kTileBytes);

				for (std::uint32_t r = 0; r < kTile; ++r) {
					for (std::uint32_t k = 0; k < kTile; ++k) {
						const float a_rk = a_data[r * kTile + k];
						for (std::uint32_t c = 0; c < kTile; ++c) {
							c_data[r * kTile + c] += a_rk * b_data[k * kTile + c];
						}
					}
				}
			}
		}

		std::printf("computing reference (untiled) GEMM for comparison\n");
		std::vector<float> reference = reference_gemm();

		std::printf("comparing tiled result against reference\n");
		bool ok = true;
		std::uint32_t first_mismatch_r = 0, first_mismatch_c = 0;
		float first_got = 0, first_want = 0;

		for (std::uint32_t ti = 0; ti < kMTiles && ok; ++ti) {
			for (std::uint32_t tj = 0; tj < kNTiles && ok; ++tj) {
				const float *c_data = as_floats(c_tiles[ti * kNTiles + tj]);
				for (std::uint32_t r = 0; r < kTile && ok; ++r) {
					const std::uint32_t global_row = ti * kTile + r;
					for (std::uint32_t c = 0; c < kTile; ++c) {
						const std::uint32_t global_col = tj * kTile + c;
						const float got = c_data[r * kTile + c];
						const float want = reference[global_row * kN + global_col];
						// Exact compare is safe here: all inputs are
						// small integers (mod 7 / mod 11) and K=128
						// terms, well within float's exact-integer
						// range (2^24) — no rounding tolerance needed.
						if (got != want) {
							ok = false;
							first_mismatch_r = global_row;
							first_mismatch_c = global_col;
							first_got = got;
							first_want = want;
							break;
						}
					}
				}
			}
		}

		if (!ok) {
			std::fprintf(stderr,
				     "FAIL: mismatch at C[%u][%u]: tiled=%.1f reference=%.1f\n",
				     first_mismatch_r, first_mismatch_c, first_got, first_want);
			return 1;
		}

		std::printf("PASS: all %u C tiles (%u x %u elements total) match the reference "
			    "GEMM exactly\n",
			    kMTiles * kNTiles, kM, kN);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
