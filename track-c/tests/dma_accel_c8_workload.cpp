// dma_accel_c8_workload.cpp
//
// C8: End-to-End Workload — validation only, no new Track C capability.
// Exercises C1-C3, C5-C7 together against a real 2-tile, K=3 accumulation
// GEMM (C_tile = A0@B0 + A1@B1 + A2@B2 for two independent output tiles),
// verified against a CPU reference computation.
//
// This is also the first time TILE_MATMUL has been driven through a real
// multi-step accumulation end to end — Track A's M11 notes explicitly
// deferred this ("hasn't yet been driven all the way through a full
// multi-tile GEMM"). C8 closes that.
//
// What's exercised, and how:
//   C1 CommandGroup   — aggregates the final validation-COPY fences (per
//                       stream — see note at that point in main() on why
//                       it's two groups, not one, given C1's frozen
//                       single-Stream scope).
//   C2 StreamObserver — non-blocking observation while polling for
//                       completion, instead of hard-blocking wait()s
//                       everywhere.
//   C3 DependencyEngine — each tile's 3-step accumulation chain (step
//                       k+1 depends_on step k) is a genuine dependency,
//                       not a manufactured one: step k+1 cannot correctly
//                       run until step k has actually written the
//                       accumulator.
//   C5 BufferPool     — each tile's per-step input buffers (A_k, B_k)
//                       come from a 4-slot pool shared across all 3
//                       steps (double-buffering: while step k is
//                       in-flight/deferred, step k+1's inputs are being
//                       prepared from the other 2 slots) — 6 total
//                       buffer-uses (3 steps x 2 inputs) served by 4
//                       physical allocations, not 6.
//   C6                — the two tile chains run on two independent
//                       Streams/sessions, each driven by its own thread.
//   C7 StreamScheduler — schedules the two FINAL, independent,
//                       dependency-free validation COPY commands (tile
//                       result -> a separate verification buffer). This
//                       is a deliberately flat, eligible-now pair of
//                       commands — exactly StreamScheduler's intended
//                       scope. It is NOT used for the two dependency
//                       chains above; C7's frozen scope boundary is that
//                       it has no DAG awareness, so the two chains are
//                       each driven by their own DependencyEngine's
//                       progress() loop — this is C8's own workload
//                       orchestration, not C7 scheduling. Stated
//                       explicitly so this boundary isn't blurred.
//   C4 CommandBatch   — NOT exercised. The tiled-GEMM execution path
//                       here has no natural independent batch-submission
//                       opportunity under the frozen single-command
//                       ioctl ABI (each TILE_MATMUL step must wait for
//                       the previous one's real completion before it can
//                       even be constructed with valid inputs — there's
//                       nothing to batch). C4 remains independently
//                       validated by its own C4 milestone tests; forcing
//                       it in here would be exercising the API for its
//                       own sake, not because the workload needs it.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -pthread -o dma_accel_c8_workload
//     dma_accel_c8_workload.cpp dma_accel_command_group.cpp
//     dma_accel_dependency.cpp dma_accel_buffer_pool.cpp
//     dma_accel_stream_observer.cpp dma_accel_stream_scheduler.cpp
//     dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_c8_workload [/dev/dma_accel0]

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dma_accel_buffer_pool.hpp"
#include "dma_accel_command_group.hpp"
#include "dma_accel_dependency.hpp"
#include "dma_accel_runtime.hpp"
#include "dma_accel_stream_observer.hpp"
#include "dma_accel_stream_scheduler.hpp"

namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
} // namespace
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace {

constexpr int kDim = DMA_ACCEL_TILE_DIM; // 32
constexpr int kElems = kDim * kDim;      // 1024
constexpr int kSteps = 3;

using Tile = std::array<float, kElems>;

// Deterministic, distinct-per-(tile,step,operand) fill pattern. Modest
// magnitude to keep 32-term dot-product accumulation well-conditioned in
// float32.
void fill_tile(Tile &t, int tile_id, int step, int operand) {
	for (int i = 0; i < kElems; ++i) {
		t[i] = 0.01f * static_cast<float>((i % 17) + tile_id * 5 + step * 3 + operand * 2 + 1);
	}
}

// Row-major 32x32 float matmul-accumulate: c += a @ b.
void cpu_matmul_accumulate(Tile &c, const Tile &a, const Tile &b) {
	for (int i = 0; i < kDim; ++i) {
		for (int j = 0; j < kDim; ++j) {
			float sum = 0.0f;
			for (int k = 0; k < kDim; ++k) {
				sum += a[i * kDim + k] * b[k * kDim + j];
			}
			c[i * kDim + j] += sum;
		}
	}
}

float max_abs_diff(const Tile &x, const Tile &y) {
	float m = 0.0f;
	for (int i = 0; i < kElems; ++i) {
		m = std::max(m, std::fabs(x[i] - y[i]));
	}
	return m;
}

struct TileChainResult {
	Tile device_result;
	Tile cpu_reference;
};

// One tile's full 3-step accumulation chain: acquires/releases pooled
// input buffers with the double-buffering discipline described in the
// header comment, drives its own DependencyEngine, and returns the
// final device-computed tile (read back from the accumulator buffer)
// alongside the CPU reference for comparison.
TileChainResult run_tile_chain(dma_accel::Stream &stream, dma_accel::DependencyEngine &engine,
				dma_accel::BufferPool &pool, int tile_id) {
	dma_accel::Buffer accumulator = stream.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
	std::memset(accumulator.data(), 0, DMA_ACCEL_TILE_MATMUL_BYTES);

	Tile cpu_c{};
	cpu_c.fill(0.0f);
	std::vector<Tile> cpu_snapshot_after_step(kSteps); // cpu_c right after each prepare_step call

	struct StepBuffers {
		std::optional<dma_accel::PooledBuffer> a, b;
	};
	std::vector<StepBuffers> held(kSteps);
	std::vector<dma_accel::PendingCommand> pendings;
	pendings.reserve(kSteps);

	auto prepare_step = [&](int step) {
		auto a = pool.acquire();
		auto b = pool.acquire();
		if (!a || !b) {
			throw std::runtime_error("tile " + std::to_string(tile_id) + " step " +
						  std::to_string(step) +
						  ": pool exhausted - double-buffering invariant violated");
		}
		Tile a_tile{}, b_tile{};
		fill_tile(a_tile, tile_id, step, 0);
		fill_tile(b_tile, tile_id, step, 1);
		std::memcpy(a->buffer().data(), a_tile.data(), DMA_ACCEL_TILE_MATMUL_BYTES);
		std::memcpy(b->buffer().data(), b_tile.data(), DMA_ACCEL_TILE_MATMUL_BYTES);
		cpu_matmul_accumulate(cpu_c, a_tile, b_tile);
		cpu_snapshot_after_step[step] = cpu_c; // CPU's running total right after this step

		dma_accel::PendingCommand pc =
			engine.defer_submit_tile_matmul(a->buffer(), 0, b->buffer(), 0, accumulator, 0);
		if (step > 0) {
			pc.depends_on(pendings[step - 1]);
		}
		held[step].a = std::move(a);
		held[step].b = std::move(b);
		pendings.push_back(std::move(pc));
	};

	// Prepare step 0 and step 1 up front (uses all 4 pool slots).
	prepare_step(0);
	prepare_step(1);

	// Drive until step 1 is submitted — at that point step 0 is
	// guaranteed complete (that's what depends_on() enforces), so it's
	// safe to release step 0's buffers and prepare step 2.
	//
	// The sleep here is load-bearing, not just an efficiency nicety:
	// progress() is deliberately non-blocking (one pump() per call), and
	// with TWO threads (this one and the other tile's) both spinning
	// with zero delay, a busy loop here can starve the host CPU badly
	// enough on a small VM that the QEMU device-model process itself
	// can't get scheduled to actually advance completion — a genuine
	// liveness bug, found by hitting it, not a hypothetical.
	while (!pendings[1].is_submitted()) {
		engine.progress();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	// DIAGNOSTIC: at this point step 0 is confirmed complete on device
	// (that's what let step 1 become submitted). Read back the
	// accumulator NOW and compare against the CPU's step-0-only partial
	// sum — localizes any divergence to step 0 specifically, instead of
	// only finding out something's wrong at the very end.
	{
		Tile step0_device{};
		std::memcpy(step0_device.data(), accumulator.data(), DMA_ACCEL_TILE_MATMUL_BYTES);
		float err = max_abs_diff(step0_device, cpu_snapshot_after_step[0]);
		std::printf("tile %d after step0: device[0]=%.4f cpu[0]=%.4f max_abs_err=%.4f\n", tile_id,
			    step0_device[0], cpu_snapshot_after_step[0][0], static_cast<double>(err));
	}
	held[0].a.reset();
	held[0].b.reset();
	prepare_step(2);

	// Drive until step 2 is submitted (implies step 1 complete — safe to
	// release step 1's buffers). Same reasoning on the sleep as above.
	while (!pendings[2].is_submitted()) {
		engine.progress();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	{
		Tile step1_device{};
		std::memcpy(step1_device.data(), accumulator.data(), DMA_ACCEL_TILE_MATMUL_BYTES);
		float err = max_abs_diff(step1_device, cpu_snapshot_after_step[1]);
		std::printf("tile %d after step1: device[0]=%.4f cpu[0]=%.4f max_abs_err=%.4f\n", tile_id,
			    step1_device[0], cpu_snapshot_after_step[1][0], static_cast<double>(err));
	}
	held[1].a.reset();
	held[1].b.reset();

	dma_accel::StreamObserver observer(stream);
	while (observer.query(pendings[2].fence()) != dma_accel::WaitResult::Completed) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	held[2].a.reset();
	held[2].b.reset();

	if (pendings[2].fence().status() != DMA_ACCEL_OK) {
		throw std::runtime_error("tile " + std::to_string(tile_id) +
					  " final step completed with non-OK status");
	}

	Tile device_c{};
	std::memcpy(device_c.data(), accumulator.data(), DMA_ACCEL_TILE_MATMUL_BYTES);
	{
		float err = max_abs_diff(device_c, cpu_snapshot_after_step[2]);
		std::printf("tile %d after step2 (final): device[0]=%.4f cpu[0]=%.4f max_abs_err=%.4f\n",
			    tile_id, device_c[0], cpu_snapshot_after_step[2][0], static_cast<double>(err));
	}

	return TileChainResult{device_c, cpu_c};
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel C8 end-to-end workload: 2-tile, K=%d GEMM accumulation ==\n", kSteps);
	std::printf("using device %s\n", device);

	try {
		dma_accel::Stream streamA(device);
		dma_accel::Stream streamB(device);

		dma_accel::DependencyEngine engineA(streamA);
		dma_accel::DependencyEngine engineB(streamB);

		// Pool capacity 4 per stream: double-buffering across the
		// 3-step chain (see prepare_step() above). 4 physical
		// allocations serve 6 total buffer-uses per tile.
		dma_accel::BufferPool poolA(streamA, DMA_ACCEL_TILE_MATMUL_BYTES, 4);
		dma_accel::BufferPool poolB(streamB, DMA_ACCEL_TILE_MATMUL_BYTES, 4);

		std::printf("-- running two independent 3-step dependency chains --\n");
		std::printf("(each chain is driven by its own DependencyEngine::progress() loop on its\n");
		std::printf(" own thread — this is C8's own workload orchestration, NOT C7\n");
		std::printf(" StreamScheduler; C7 has no dependency-graph awareness by design)\n");

		TileChainResult resultA, resultB;
		std::exception_ptr excA, excB;

		std::thread threadA([&]() {
			try {
				resultA = run_tile_chain(streamA, engineA, poolA, 0);
			} catch (...) {
				excA = std::current_exception();
			}
		});
		std::thread threadB([&]() {
			try {
				resultB = run_tile_chain(streamB, engineB, poolB, 1);
			} catch (...) {
				excB = std::current_exception();
			}
		});
		threadA.join();
		threadB.join();

		if (excA) {
			std::rethrow_exception(excA);
		}
		if (excB) {
			std::rethrow_exception(excB);
		}

		float errA = max_abs_diff(resultA.device_result, resultA.cpu_reference);
		float errB = max_abs_diff(resultB.device_result, resultB.cpu_reference);
		std::printf("tile A: max_abs_error = %g\n", static_cast<double>(errA));
		std::printf("tile B: max_abs_error = %g\n", static_cast<double>(errB));

		constexpr float kTolerance = 1e-2f;
		bool correctA = errA < kTolerance;
		bool correctB = errB < kTolerance;
		std::printf("tile A dependency chain (A0 -> A1 -> A2): %s\n", correctA ? "PASS" : "FAIL");
		std::printf("tile B dependency chain (B0 -> B1 -> B2): %s\n", correctB ? "PASS" : "FAIL");

		if (!correctA || !correctB) {
			std::fprintf(stderr, "FAIL: GEMM accumulation result incorrect\n");
			return 1;
		}

		std::printf("pool A: capacity=%zu (4 physical allocations served %d buffer-uses)\n",
			    poolA.capacity(), kSteps * 2);
		std::printf("pool B: capacity=%zu (4 physical allocations served %d buffer-uses)\n",
			    poolB.capacity(), kSteps * 2);

		// -- C7: schedule the two final, independent, dependency-free
		// validation COPY commands. This is the deliberately flat,
		// eligible-now pair StreamScheduler is actually meant for. --
		std::printf("-- scheduling final validation COPY via C7 StreamScheduler --\n");
		dma_accel::Buffer verifyA = streamA.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		dma_accel::Buffer verifyB = streamB.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		dma_accel::Buffer accumA_readback = streamA.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		dma_accel::Buffer accumB_readback = streamB.alloc_buffer(DMA_ACCEL_TILE_MATMUL_BYTES);
		std::memcpy(accumA_readback.data(), resultA.device_result.data(), DMA_ACCEL_TILE_MATMUL_BYTES);
		std::memcpy(accumB_readback.data(), resultB.device_result.data(), DMA_ACCEL_TILE_MATMUL_BYTES);

		dma_accel::StreamScheduler scheduler;
		std::size_t idxA = scheduler.add_stream(streamA);
		std::size_t idxB = scheduler.add_stream(streamB);
		scheduler.enqueue(idxA, [&]() -> dma_accel::Fence {
			return streamA.submit(OPCODE_COPY, accumA_readback, 0, verifyA, 0,
					       DMA_ACCEL_TILE_MATMUL_BYTES);
		});
		scheduler.enqueue(idxB, [&]() -> dma_accel::Fence {
			return streamB.submit(OPCODE_COPY, accumB_readback, 0, verifyB, 0,
					       DMA_ACCEL_TILE_MATMUL_BYTES);
		});
		std::vector<dma_accel::ScheduledResult> scheduled = scheduler.run();
		std::printf("scheduled %zu validation COPY commands\n", scheduled.size());

		// -- C1: aggregate the final fences. NOTE: CommandGroup is bound
		// to exactly one Stream (C1's frozen scope). Since the two final
		// Fences come from two DIFFERENT Streams, they cannot both go
		// into a single CommandGroup without violating that boundary —
		// this is the C1/C6 boundary already frozen in the spec. So:
		// two groups, one per stream, not one combined group. --
		dma_accel::CommandGroup groupA(streamA);
		dma_accel::CommandGroup groupB(streamB);
		for (auto &r : scheduled) {
			if (r.stream_index == idxA) {
				groupA.add(std::move(r.fence));
			} else {
				groupB.add(std::move(r.fence));
			}
		}
		groupA.seal();
		groupB.seal();
		dma_accel::GroupStatus statusA = groupA.wait(2000);
		dma_accel::GroupStatus statusB = groupB.wait(2000);

		if (statusA != dma_accel::GroupStatus::AllOk || statusB != dma_accel::GroupStatus::AllOk) {
			std::fprintf(stderr, "FAIL: validation COPY did not complete OK\n");
			return 1;
		}

		if (std::memcmp(accumA_readback.data(), verifyA.data(), DMA_ACCEL_TILE_MATMUL_BYTES) != 0 ||
		    std::memcmp(accumB_readback.data(), verifyB.data(), DMA_ACCEL_TILE_MATMUL_BYTES) != 0) {
			std::fprintf(stderr, "FAIL: validation COPY data mismatch\n");
			return 1;
		}

		std::printf("CommandGroup aggregation: groupA=%s groupB=%s\n",
			    statusA == dma_accel::GroupStatus::AllOk ? "AllOk" : "FAIL",
			    statusB == dma_accel::GroupStatus::AllOk ? "AllOk" : "FAIL");

		std::printf("C4 CommandBatch: not exercised by this workload (see header comment) - "
			    "independently validated by its own C4 milestone tests\n");

		std::printf("PASS: C8 end-to-end workload verified - 2-tile K=%d GEMM accumulation "
			    "correct on both tiles, exercising C1/C2/C3/C5/C6/C7 together, C4 "
			    "independently validated separately\n",
			    kSteps);
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
