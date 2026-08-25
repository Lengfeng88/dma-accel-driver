// dma_accel_gemm_stage3.cpp
//
// M11 stage 3: adds non-divisible matrix dimensions on top of stage 2's
// K-accumulation. M=300, K=200, N=260 against tile=128 — none of the
// three dimensions divide evenly, so every tile row/col has at least one
// boundary tile to exercise.
//
// Approach: always allocate full TILE x TILE buffers and zero-pad the
// out-of-range region when filling a boundary tile. The multiply-
// accumulate inner loop is byte-for-byte identical to stage 2's — it
// always operates on a full tile, and padding contributes exactly zero,
// so no special-cased partial-tile arithmetic is needed. The only new
// logic is (a) computing each tile's valid (in-bounds) row/col count and
// zero-filling the rest, and (b) when checking results, only comparing
// each C tile's valid region against the reference — the padded region
// doesn't correspond to any real matrix element.
//
// Still deliberately NOT doing: any device-side COPY (see stage 1's
// header comment for why — still true here).
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra -o dma_accel_gemm_stage3 dma_accel_gemm_stage3.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_gemm_stage3 [/dev/dma_accel0]

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

#include "dma_accel_runtime.hpp"

namespace {

constexpr std::uint32_t kTile = 128;
constexpr std::uint32_t kM = 300; // 2 full tiles + 44-row remainder
constexpr std::uint32_t kK = 200; // 1 full tile + 72-row remainder
constexpr std::uint32_t kN = 260; // 2 full tiles + 4-col remainder

constexpr std::uint32_t kTileBytes = kTile * kTile * sizeof(float);

// Ceiling division — how many tiles to cover a dimension that doesn't
// divide evenly. This (not kDim / kTile) is the one new piece of index
// arithmetic this stage adds.
constexpr std::uint32_t ceil_div(std::uint32_t a, std::uint32_t b) { return (a + b - 1) / b; }

constexpr std::uint32_t kMTiles = ceil_div(kM, kTile); // 3
constexpr std::uint32_t kNTiles = ceil_div(kN, kTile); // 3
constexpr std::uint32_t kKTiles = ceil_div(kK, kTile); // 2

// How many rows/cols of tile index `t` (0-based) along a dimension of
// total size `dim` are actually real data, vs. zero-padding. Every tile
// except possibly the last is a full kTile; the last is whatever's left.
std::uint32_t valid_extent(std::uint32_t t, std::uint32_t dim) {
	const std::uint32_t start = t * kTile;
	return std::min(kTile, dim - start);
}

float *as_floats(dma_accel::Buffer &buf) { return reinterpret_cast<float *>(buf.data()); }

// Same fill functions as stages 1/2 — unchanged, so any mismatch can
// only come from the new padding/boundary logic.
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

	std::printf("== dma-accel M11 stage 3: non-divisible dimensions check ==\n");
	std::printf("A: %ux%u, B: %ux%u, C: %ux%u, tile: %ux%u (%ux%u A-tile grid, %ux%u B-tile "
		    "grid, %ux%u C-tile grid — none of M/K/N divide evenly by %u)\n",
		    kM, kK, kK, kN, kM, kN, kTile, kTile, kMTiles, kKTiles, kKTiles, kNTiles, kMTiles,
		    kNTiles, kTile);

	try {
		dma_accel::Stream stream(device);

		// A tiles: kMTiles x kKTiles grid. Boundary tiles (last row-block
		// and/or last K-block) are zero-padded beyond their valid extent.
		std::vector<dma_accel::Buffer> a_tiles;
		a_tiles.reserve(kMTiles * kKTiles);
		for (std::uint32_t ti = 0; ti < kMTiles; ++ti) {
			const std::uint32_t valid_rows = valid_extent(ti, kM);
			for (std::uint32_t tk = 0; tk < kKTiles; ++tk) {
				const std::uint32_t valid_k = valid_extent(tk, kK);
				a_tiles.push_back(stream.alloc_buffer(kTileBytes));
				float *dst = as_floats(a_tiles.back());
				std::memset(dst, 0, kTileBytes); // pad first, overwrite valid region below
				for (std::uint32_t r = 0; r < valid_rows; ++r) {
					const std::uint32_t global_row = ti * kTile + r;
					for (std::uint32_t k = 0; k < valid_k; ++k) {
						const std::uint32_t global_k = tk * kTile + k;
						dst[r * kTile + k] = a_value(global_row, global_k);
					}
				}
			}
		}
		auto a_tile_at = [&](std::uint32_t ti, std::uint32_t tk) -> dma_accel::Buffer & {
			return a_tiles[ti * kKTiles + tk];
		};

		// B tiles: kKTiles x kNTiles grid, same padding treatment.
		std::vector<dma_accel::Buffer> b_tiles;
		b_tiles.reserve(kKTiles * kNTiles);
		for (std::uint32_t tk = 0; tk < kKTiles; ++tk) {
			const std::uint32_t valid_k = valid_extent(tk, kK);
			for (std::uint32_t tj = 0; tj < kNTiles; ++tj) {
				const std::uint32_t valid_cols = valid_extent(tj, kN);
				b_tiles.push_back(stream.alloc_buffer(kTileBytes));
				float *dst = as_floats(b_tiles.back());
				std::memset(dst, 0, kTileBytes);
				for (std::uint32_t k = 0; k < valid_k; ++k) {
					const std::uint32_t global_k = tk * kTile + k;
					for (std::uint32_t c = 0; c < valid_cols; ++c) {
						const std::uint32_t global_col = tj * kTile + c;
						dst[k * kTile + c] = b_value(global_k, global_col);
					}
				}
			}
		}
		auto b_tile_at = [&](std::uint32_t tk, std::uint32_t tj) -> dma_accel::Buffer & {
			return b_tiles[tk * kNTiles + tj];
		};

		std::printf("computing %u C tiles, each accumulating over %u K-tiles (full-tile "
			    "arithmetic throughout — padding handles the boundary, not the loops)\n",
			    kMTiles * kNTiles, kKTiles);

		// C tile compute: byte-for-byte the same inner loop as stage 2.
		// No boundary special-casing here at all — that's the point.
		std::vector<dma_accel::Buffer> c_tiles;
		c_tiles.reserve(kMTiles * kNTiles);
		for (std::uint32_t ti = 0; ti < kMTiles; ++ti) {
			for (std::uint32_t tj = 0; tj < kNTiles; ++tj) {
				c_tiles.push_back(stream.alloc_buffer(kTileBytes));
				float *c_data = as_floats(c_tiles.back());
				std::memset(c_data, 0, kTileBytes);

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

		std::printf("comparing each C tile's VALID region against the reference (padded "
			    "region doesn't correspond to a real matrix element, so it's skipped)\n");
		bool ok = true;
		std::uint32_t first_mismatch_r = 0, first_mismatch_c = 0;
		float first_got = 0, first_want = 0;

		for (std::uint32_t ti = 0; ti < kMTiles && ok; ++ti) {
			const std::uint32_t valid_rows = valid_extent(ti, kM);
			for (std::uint32_t tj = 0; tj < kNTiles && ok; ++tj) {
				const std::uint32_t valid_cols = valid_extent(tj, kN);
				const float *c_data = as_floats(c_tiles[ti * kNTiles + tj]);
				for (std::uint32_t r = 0; r < valid_rows && ok; ++r) {
					const std::uint32_t global_row = ti * kTile + r;
					for (std::uint32_t c = 0; c < valid_cols; ++c) {
						const std::uint32_t global_col = tj * kTile + c;
						const float got = c_data[r * kTile + c];
						const float want = reference[global_row * kN + global_col];
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

		std::printf("PASS: all valid regions across %u C tiles (%u x %u real elements "
			    "total) match the reference GEMM exactly, despite M/K/N=%u/%u/%u not "
			    "dividing evenly by tile=%u\n",
			    kMTiles * kNTiles, kM, kN, kM, kK, kN, kTile);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
