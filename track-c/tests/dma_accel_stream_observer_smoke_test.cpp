// dma_accel_stream_observer_smoke_test.cpp
//
// C2 smoke test: verifies StreamObserver's non-blocking query() and
// timeout-bounded wait() against real device timing, not just API shape.
//
// Two of these checks are timing-dependent by nature (that's the whole
// point of C2 — observing genuine in-flight state), so they're worth
// explaining rather than treating as flaky noise if they ever fail:
//
//   - query() immediately after submit() is expected to see Pending,
//     because Track A's driver is IRQ-driven (see Track A M3-M5):
//     submit() enqueues and returns before the device has necessarily
//     even started the transfer, let alone finished it. If this
//     ever reliably comes back Completed instead, that's a genuine
//     signal the device got fast enough (or emulation timing changed)
//     that this assumption needs revisiting — not something to paper
//     over by loosening the check.
//   - wait(fence, 0ms) immediately after submit() is expected to see
//     Pending for the same reason, and is otherwise deterministic
//     (0ms means "check once, don't block" via the underlying poll()
//     with a zero timeout — no race window to speak of).
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_stream_observer_smoke_test
//     dma_accel_stream_observer_smoke_test.cpp dma_accel_stream_observer.cpp
//     dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_stream_observer_smoke_test [/dev/dma_accel0]

#include <cstdio>
#include <cstring>
#include <exception>

#include "dma_accel_runtime.hpp"
#include "dma_accel_stream_observer.hpp"

// Same shim as the other test/runtime files.
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

const char *result_name(dma_accel::WaitResult r) {
	return r == dma_accel::WaitResult::Completed ? "Completed" : "Pending";
}

// Submits one COPY command and returns its Fence, plus the two buffers
// (so the caller can verify data correctness once it's known complete).
dma_accel::Fence submit_copy(dma_accel::Stream &stream, dma_accel::Buffer &src,
			      dma_accel::Buffer &dst, int fill_seed) {
	for (std::uint32_t b = 0; b < kBufSize; ++b) {
		src.data()[b] = static_cast<std::uint8_t>((b * 31 + 7 + fill_seed * 17) & 0xFF);
	}
	std::memset(dst.data(), 0, kBufSize);
	return stream.submit(OPCODE_COPY, src, 0, dst, 0, kBufSize);
}

bool check_copy_correct(const dma_accel::Buffer &src, const dma_accel::Buffer &dst) {
	return std::memcmp(src.data(), dst.data(), kBufSize) == 0;
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel C2 StreamObserver smoke test ==\n");

	try {
		dma_accel::Stream stream(device);
		std::printf("opened %s\n", device);

		dma_accel::StreamObserver observer(stream);
		bool ok = true;

		// -- 1. query() immediately after submit(): expect Pending --
		{
			std::printf("-- query() immediately after submit() --\n");
			dma_accel::Buffer src = stream.alloc_buffer(kBufSize);
			dma_accel::Buffer dst = stream.alloc_buffer(kBufSize);
			dma_accel::Fence fence = submit_copy(stream, src, dst, 1);

			dma_accel::WaitResult r = observer.query(fence);
			std::printf("query() -> %s\n", result_name(r));
			if (r != dma_accel::WaitResult::Pending) {
				std::fprintf(stderr,
					     "FAIL: expected Pending immediately after submit(), "
					     "got Completed (see header comment on why this "
					     "assumption might need revisiting rather than "
					     "just loosening the check)\n");
				ok = false;
			} else {
				std::printf("PASS: query() correctly observed in-flight state\n");
			}

			// Drain it properly and verify correctness before moving on.
			dma_accel::WaitResult done = observer.wait(fence);
			if (done != dma_accel::WaitResult::Completed) {
				std::fprintf(stderr, "FAIL: indefinite wait() did not return Completed\n");
				ok = false;
			} else if (fence.status() != DMA_ACCEL_OK) {
				std::fprintf(stderr, "FAIL: status=%u, want DMA_ACCEL_OK\n", fence.status());
				ok = false;
			} else if (!check_copy_correct(src, dst)) {
				std::fprintf(stderr, "FAIL: data mismatch after COPY\n");
				ok = false;
			} else {
				std::printf("PASS: command completed correctly after wait()\n");
			}
		}

		// -- 2. wait(fence, 0ms) immediately after submit(): expect Pending --
		{
			std::printf("-- wait(fence, 0ms) immediately after submit() --\n");
			dma_accel::Buffer src = stream.alloc_buffer(kBufSize);
			dma_accel::Buffer dst = stream.alloc_buffer(kBufSize);
			dma_accel::Fence fence = submit_copy(stream, src, dst, 2);

			dma_accel::WaitResult r = observer.wait(fence, std::chrono::milliseconds(0));
			std::printf("wait(0ms) -> %s\n", result_name(r));
			if (r != dma_accel::WaitResult::Pending) {
				std::fprintf(stderr, "FAIL: expected Pending from a 0ms wait immediately "
						      "after submit()\n");
				ok = false;
			} else {
				std::printf("PASS: 0ms wait correctly did not block and saw Pending\n");
			}

			// Drain properly with a generous timeout this time.
			dma_accel::WaitResult done = observer.wait(fence, std::chrono::milliseconds(2000));
			if (done != dma_accel::WaitResult::Completed) {
				std::fprintf(stderr, "FAIL: 2000ms wait() did not return Completed\n");
				ok = false;
			} else if (fence.status() != DMA_ACCEL_OK) {
				std::fprintf(stderr, "FAIL: status=%u, want DMA_ACCEL_OK\n", fence.status());
				ok = false;
			} else if (!check_copy_correct(src, dst)) {
				std::fprintf(stderr, "FAIL: data mismatch after COPY\n");
				ok = false;
			} else {
				std::printf("PASS: command completed correctly within timeout\n");
			}
		}

		// -- 3. query() after a real wait(): expect Completed, no extra I/O surprises --
		{
			std::printf("-- query() after the fence is already known-ready --\n");
			dma_accel::Buffer src = stream.alloc_buffer(kBufSize);
			dma_accel::Buffer dst = stream.alloc_buffer(kBufSize);
			dma_accel::Fence fence = submit_copy(stream, src, dst, 3);

			observer.wait(fence); // drain indefinitely first
			dma_accel::WaitResult r = observer.query(fence);
			if (r != dma_accel::WaitResult::Completed) {
				std::fprintf(stderr, "FAIL: query() on an already-ready fence returned Pending\n");
				ok = false;
			} else {
				std::printf("PASS: query() on an already-ready fence correctly returned Completed\n");
			}
		}

		if (!ok) {
			std::fprintf(stderr, "FAIL: one or more checks failed, see above\n");
			return 1;
		}

		std::printf("PASS: StreamObserver verified — non-blocking query() and "
			    "timeout-bounded wait() both correctly reflect real device state\n");
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
