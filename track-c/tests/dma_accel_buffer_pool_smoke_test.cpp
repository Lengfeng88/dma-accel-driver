// dma_accel_buffer_pool_smoke_test.cpp
//
// C5 smoke test:
//   1. Basic checkout/exhaustion: acquire() up to capacity, verify
//      counts, verify acquire() returns nullopt once exhausted.
//   2. Release and reacquire: dropping a PooledBuffer returns it to the
//      pool; the SAME underlying buffer_id comes back out on the next
//      acquire() — direct proof this is reuse, not fresh device
//      allocation (which is the entire reason C5 exists — see header).
//   3. Real usage cycle: acquire two pooled buffers, submit a real COPY
//      between them, wait for completion, verify data, THEN release —
//      demonstrating the correct discipline (release only after the
//      Fence is ready) that this class does not enforce for you.
//   4. Quota-saving proof: run several acquire/submit/wait/release
//      cycles through a pool of just 2 buffers and confirm the
//      buffer_id set never grows past 2 — directly the problem C3's
//      testing actually hit (Track B's 16-buffer/session quota) and
//      the concrete justification for this milestone.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_buffer_pool_smoke_test
//     dma_accel_buffer_pool_smoke_test.cpp dma_accel_buffer_pool.cpp
//     dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_buffer_pool_smoke_test [/dev/dma_accel0]

#include <cstdio>
#include <cstring>
#include <exception>
#include <set>

#include "dma_accel_buffer_pool.hpp"
#include "dma_accel_runtime.hpp"

namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
} // namespace
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace {

constexpr std::uint32_t kBufSize = 4096;

bool test_checkout_exhaustion(const char *device) {
	std::printf("-- 1. checkout / exhaustion --\n");
	dma_accel::Stream stream(device);
	dma_accel::BufferPool pool(stream, kBufSize, 3);

	if (pool.capacity() != 3 || pool.available() != 3 || pool.checked_out() != 0) {
		std::fprintf(stderr, "FAIL: unexpected initial pool counts\n");
		return false;
	}

	auto a = pool.acquire();
	auto b = pool.acquire();
	auto c = pool.acquire();
	if (!a || !b || !c) {
		std::fprintf(stderr, "FAIL: expected 3 successful acquires from a pool of 3\n");
		return false;
	}
	if (pool.available() != 0 || pool.checked_out() != 3) {
		std::fprintf(stderr, "FAIL: pool counts wrong after 3 acquires (available=%zu checked_out=%zu)\n",
			     pool.available(), pool.checked_out());
		return false;
	}

	auto d = pool.acquire();
	if (d.has_value()) {
		std::fprintf(stderr, "FAIL: acquire() on an exhausted pool should return nullopt\n");
		return false;
	}
	std::printf("PASS: pool correctly exhausted after capacity acquires, further acquire() is nullopt\n");
	return true;
}

bool test_release_and_reacquire(const char *device) {
	std::printf("-- 2. release returns the SAME buffer_id --\n");
	dma_accel::Stream stream(device);
	dma_accel::BufferPool pool(stream, kBufSize, 1);

	std::uint32_t first_id;
	{
		auto a = pool.acquire();
		if (!a) {
			std::fprintf(stderr, "FAIL: first acquire() unexpectedly failed\n");
			return false;
		}
		first_id = a->buffer().buffer_id();
	} // `a` destroyed here — returns to pool

	if (pool.available() != 1) {
		std::fprintf(stderr, "FAIL: buffer was not returned to the pool after PooledBuffer destruction\n");
		return false;
	}

	auto b = pool.acquire();
	if (!b) {
		std::fprintf(stderr, "FAIL: reacquire after release unexpectedly failed\n");
		return false;
	}
	if (b->buffer().buffer_id() != first_id) {
		std::fprintf(stderr, "FAIL: reacquire returned a different buffer_id (%u vs %u) — this is not "
				      "reuse, defeats the entire point of C5\n",
			     b->buffer().buffer_id(), first_id);
		return false;
	}
	std::printf("PASS: released buffer correctly reused (buffer_id=%u both times)\n", first_id);
	return true;
}

bool test_real_usage_cycle(const char *device) {
	std::printf("-- 3. real COPY through pooled buffers, correct release discipline --\n");
	dma_accel::Stream stream(device);
	dma_accel::BufferPool pool(stream, kBufSize, 2);

	auto src = pool.acquire();
	auto dst = pool.acquire();
	if (!src || !dst) {
		std::fprintf(stderr, "FAIL: expected 2 successful acquires\n");
		return false;
	}

	for (std::uint32_t i = 0; i < kBufSize; ++i) {
		src->buffer().data()[i] = static_cast<std::uint8_t>((i * 19 + 3) & 0xFF);
	}
	std::memset(dst->buffer().data(), 0, kBufSize);

	dma_accel::Fence fence =
		stream.submit(OPCODE_COPY, src->buffer(), 0, dst->buffer(), 0, kBufSize);

	// Correct discipline: wait for the Fence BEFORE letting src/dst go
	// out of scope and return to the pool. This is exactly the hazard
	// documented in the header — this test exercises the safe path.
	if (!stream.wait(fence, 2000)) {
		std::fprintf(stderr, "FAIL: COPY did not complete in time\n");
		return false;
	}
	if (fence.status() != DMA_ACCEL_OK) {
		std::fprintf(stderr, "FAIL: status=%u, want OK\n", fence.status());
		return false;
	}
	if (std::memcmp(src->buffer().data(), dst->buffer().data(), kBufSize) != 0) {
		std::fprintf(stderr, "FAIL: data mismatch after pooled-buffer COPY\n");
		return false;
	}
	std::printf("PASS: COPY through pooled buffers completed correctly, released only after Fence ready\n");
	return true;
}

bool test_quota_saving(const char *device) {
	std::printf("-- 4. quota-saving: many cycles through a pool of 2 --\n");
	dma_accel::Stream stream(device);
	dma_accel::BufferPool pool(stream, kBufSize, 2);

	std::set<std::uint32_t> seen_ids;
	constexpr int kCycles = 5;

	for (int cycle = 0; cycle < kCycles; ++cycle) {
		auto src = pool.acquire();
		auto dst = pool.acquire();
		if (!src || !dst) {
			std::fprintf(stderr, "FAIL: cycle %d: expected 2 successful acquires\n", cycle);
			return false;
		}
		seen_ids.insert(src->buffer().buffer_id());
		seen_ids.insert(dst->buffer().buffer_id());

		for (std::uint32_t i = 0; i < kBufSize; ++i) {
			src->buffer().data()[i] = static_cast<std::uint8_t>((i + cycle * 7) & 0xFF);
		}
		std::memset(dst->buffer().data(), 0, kBufSize);

		dma_accel::Fence fence =
			stream.submit(OPCODE_COPY, src->buffer(), 0, dst->buffer(), 0, kBufSize);
		if (!stream.wait(fence, 2000) || fence.status() != DMA_ACCEL_OK) {
			std::fprintf(stderr, "FAIL: cycle %d: COPY did not complete correctly\n", cycle);
			return false;
		}
		if (std::memcmp(src->buffer().data(), dst->buffer().data(), kBufSize) != 0) {
			std::fprintf(stderr, "FAIL: cycle %d: data mismatch\n", cycle);
			return false;
		}
		// src/dst released here at end of loop iteration, only after
		// the Fence was confirmed ready above.
	}

	if (seen_ids.size() != 2) {
		std::fprintf(stderr, "FAIL: expected exactly 2 distinct buffer_ids across %d cycles, saw %zu — "
				      "reuse isn't actually happening\n",
			     kCycles, seen_ids.size());
		return false;
	}
	std::printf("PASS: %d cycles of acquire/submit/wait/release through a pool of 2 used only "
		    "2 distinct buffer_ids total (not %d) — this is the actual quota pressure C5 exists to relieve\n",
		    kCycles, kCycles * 2);
	return true;
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel C5 BufferPool smoke test ==\n");
	std::printf("using device %s (each scenario opens its own Stream/session)\n", device);

	try {
		bool ok = true;
		ok = test_checkout_exhaustion(device) && ok;
		ok = test_release_and_reacquire(device) && ok;
		ok = test_real_usage_cycle(device) && ok;
		ok = test_quota_saving(device) && ok;

		if (!ok) {
			std::fprintf(stderr, "FAIL: one or more checks failed, see above\n");
			return 1;
		}

		std::printf("PASS: BufferPool verified — checkout/exhaustion, real reuse, correct release "
			    "discipline, and quota-saving all correct\n");
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
