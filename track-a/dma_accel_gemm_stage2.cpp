// dma_accel_gemm_stage2.cpp
//
// M11 stage 2: adds K-direction accumulation on top of stage 1's proven
// tile indexing. Each C tile now depends on kKTiles (A tile, B tile)
// pairs, summed — the case every real-sized GEMM actually needs (K
// bigger than one tile). Still deliberately NOT doing: non-divisible
// dimensions, or any device-side COPY (see dma_accel_gemm_stage1.cpp for
// why COPY has no forcing function yet — still true here).
//
// Shape: M = N = 256, K = 256 (2 K-tiles now, vs. stage 1's 1), tile
// 128x128. Buffer count: A tiles 2x2=4, B tiles 2x2=4, C tiles 2x2=4 =
// 12 total, still well under MAX_BUFFERS (64).
//
// Build (use -O2 — real triple-nested-loop matmul, now with an outer
// accumulation loop too):
//   g++ -std=c++17 -O2 -Wall -Wextra -o dma_accel_gemm_stage2 dma_accel_gemm_stage2.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_gemm_stage2 [/dev/dma_accel0]

#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

#include "dma_accel_runtime.hpp"

namespace {

constexpr std::uint32_t kTile = 128;
constexpr std::uint32_t kM = 256;
constexpr std::uint32_t kK = 256; // now 2 tiles' worth — the new variable this stage adds
constexpr std::uint32_t kN = 256;

constexpr std::uint32_t kTileBytes = kTile * kTile * sizeof(float);
constexpr std::uint32_t kMTiles = kM / kTile; // 2
constexpr std::uint32_t kNTiles = kN / kTile; // 2
constexpr std::uint32_t kKTiles = kK / kTile; // 2 — the dimension stage 1 fixed at 1

float *as_floats(dma_accel::Buffer &buf) { return reinterpret_cast<float *>(buf.data()); }

// Same fill functions as stage 1 — unchanged so a diff between the two
// stages' behavior can only come from the accumulation logic, not from
// also having changed what data is being multiplied.
float a_value(std::uint32_t global_row, std::uint32_t global_col) {
	return static_cast<float>((global_row * 3 + global_col) % 7);
}
float b_value(std::uint32_t global_row, std::uint32_t global_col) {
	return static_cast<float>((global_row + global_col * 5) % 11);
}

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

	std::printf("== dma-accel M11 stage 2: K-direction accumulation check ==\n");
	std::printf("A: %ux%u, B: %ux%u, C: %ux%u, tile: %ux%u (%u A-tiles, %u B-tiles, %u "
		    "K-tiles per C-tile, %u C-tiles)\n",
		    kM, kK, kK, kN, kM, kN, kTile, kTile, kMTiles * kKTiles, kKTiles * kNTiles,
		    kKTiles, kMTiles * kNTiles);

	try {
		dma_accel::Stream stream(device);

		// A tiles: kMTiles x kKTiles grid (row-block, K-block).
		std::vector<dma_accel::Buffer> a_tiles;
		a_tiles.reserve(kMTiles * kKTiles);
		for (std::uint32_t ti = 0; ti < kMTiles; ++ti) {
			for (std::uint32_t tk = 0; tk < kKTiles; ++tk) {
				a_tiles.push_back(stream.alloc_buffer(kTileBytes));
				float *dst = as_floats(a_tiles.back());
				for (std::uint32_t r = 0; r < kTile; ++r) {
					const std::uint32_t global_row = ti * kTile + r;
					for (std::uint32_t k = 0; k < kTile; ++k) {
						const std::uint32_t global_k = tk * kTile + k;
						dst[r * kTile + k] = a_value(global_row, global_k);
					}
				}
			}
		}
		auto a_tile_at = [&](std::uint32_t ti, std::uint32_t tk) -> dma_accel::Buffer & {
			return a_tiles[ti * kKTiles + tk];
		};

		// B tiles: kKTiles x kNTiles grid (K-block, col-block).
		std::vector<dma_accel::Buffer> b_tiles;
		b_tiles.reserve(kKTiles * kNTiles);
		for (std::uint32_t tk = 0; tk < kKTiles; ++tk) {
			for (std::uint32_t tj = 0; tj < kNTiles; ++tj) {
				b_tiles.push_back(stream.alloc_buffer(kTileBytes));
				float *dst = as_floats(b_tiles.back());
				for (std::uint32_t k = 0; k < kTile; ++k) {
					const std::uint32_t global_k = tk * kTile + k;
					for (std::uint32_t c = 0; c < kTile; ++c) {
						const std::uint32_t global_col = tj * kTile + c;
						dst[k * kTile + c] = b_value(global_k, global_col);
					}
				}
			}
		}
		auto b_tile_at = [&](std::uint32_t tk, std::uint32_t tj) -> dma_accel::Buffer & {
			return b_tiles[tk * kNTiles + tj];
		};

		std::printf("computing %u C tiles, each accumulating over %u K-tiles\n",
			    kMTiles * kNTiles, kKTiles);

		std::vector<dma_accel::Buffer> c_tiles;
		c_tiles.reserve(kMTiles * kNTiles);
		for (std::uint32_t ti = 0; ti < kMTiles; ++ti) {
			for (std::uint32_t tj = 0; tj < kNTiles; ++tj) {
				c_tiles.push_back(stream.alloc_buffer(kTileBytes));
				float *c_data = as_floats(c_tiles.back());
				std::memset(c_data, 0, kTileBytes);

				// The new logic vs. stage 1: sum contributions from
				// every K-tile instead of computing exactly one.
				for (std::uint32_t tk = 0; tk < kKTiles; ++tk) {
					const float *a_data = as_floats(a_tile_at(ti, tk));
					const float *b_data = as_floats(b_tile_at(tk, tj));

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
		}

		std::printf("computing reference (untiled) GEMM for comparison\n");
		std::vector<float> reference = reference_gemm();

		std::printf("comparing tiled+accumulated result against reference\n");
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
						// Still exact-comparable: inputs are small
						// integers (mod 7 / mod 11), and even summed
						// over kKTiles=2 tiles of 128 terms each
						// (256 total), the result stays well within
						// float's exact-integer range (2^24).
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

		std::printf("PASS: all %u C tiles (%u x %u elements total, each summed over %u "
			    "K-tiles) match the reference GEMM exactly\n",
			    kMTiles * kNTiles, kM, kN, kKTiles);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
