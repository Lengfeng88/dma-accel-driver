// dma_accel_context_smoke_test.cpp
//
// M11 smoke test for the Context abstraction. Two things to prove:
//
//   (1) Context is a correct lift of Stream — same-Context alloc/submit/
//       wait works exactly like Stream did directly (M10's smoke test
//       already proved Stream itself is correct; this just confirms
//       Context doesn't break that by wrapping it).
//   (2) The whole point of Context over bare Stream: passing a Buffer
//       from one Context into another Context's submit() is caught
//       in-process as std::logic_error, before any syscall — not as a
//       std::system_error(EPERM) surfacing from inside the kernel's
//       M10.5 ownership check. Both layers reject it; this test checks
//       we get the more useful one.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_context_smoke_test \
//       dma_accel_context_smoke_test.cpp dma_accel_context.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_context_smoke_test [/dev/dma_accel0]

#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

#include "dma_accel_context.hpp"

namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
} // namespace
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace {

int g_failures = 0;

void report(bool ok, const std::string &what) {
	std::printf("  %s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
	if (!ok) {
		++g_failures;
	}
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel M11 Context smoke test ==\n\n");

	dma_accel::Context ctx_a("client-A", device);
	dma_accel::Context ctx_b("client-B", device);

	report(ctx_a.name() == "client-A" && ctx_b.name() == "client-B",
	       "Context::name() returns what was passed at construction");

	// --- (1) Same-Context submit still works, same as bare Stream -------
	std::printf("\n[1] same-Context submit (Context must not break what Stream already did)\n");
	{
		dma_accel::Buffer src = ctx_a.alloc_buffer(4096);
		dma_accel::Buffer dst = ctx_a.alloc_buffer(4096);
		std::memset(src.data(), 0xAA, 4096);
		std::memset(dst.data(), 0x00, 4096);

		try {
			dma_accel::Fence fence = ctx_a.submit(OPCODE_COPY, src, 0, dst, 0, 4096);
			bool completed = ctx_a.wait(fence, 1000);
			report(completed && fence.status() == DMA_ACCEL_OK,
			       "same-Context COPY completes with OK status");
			report(std::memcmp(src.data(), dst.data(), 4096) == 0,
			       "dst matches src after same-Context COPY");
		} catch (const std::exception &e) {
			report(false, std::string("same-Context submit threw unexpectedly: ") + e.what());
		}
	}

	// --- (2) Cross-Context Buffer use is caught in userspace -------------
	std::printf("\n[2] cross-Context submit (must be caught as std::logic_error, no syscall)\n");
	{
		dma_accel::Buffer b_src = ctx_b.alloc_buffer(4096);
		dma_accel::Buffer b_dst = ctx_b.alloc_buffer(4096);
		dma_accel::Buffer a_dst = ctx_a.alloc_buffer(4096);

		bool threw_logic_error = false;
		bool threw_something_else = false;
		try {
			// ctx_a submitting with ctx_b's buffers as src/dst —
			// should never reach the kernel at all.
			(void)ctx_a.submit(OPCODE_COPY, b_src, 0, a_dst, 0, 4096);
		} catch (const std::logic_error &e) {
			threw_logic_error = true;
			std::printf("  (caught, as expected: %s)\n", e.what());
		} catch (const std::exception &e) {
			threw_something_else = true;
			std::printf("  (caught WRONG exception type: %s)\n", e.what());
		}
		report(threw_logic_error && !threw_something_else,
		       "ctx_a.submit() with ctx_b's src buffer throws std::logic_error");

		threw_logic_error = false;
		threw_something_else = false;
		try {
			(void)ctx_a.submit(OPCODE_COPY, a_dst, 0, b_dst, 0, 4096);
		} catch (const std::logic_error &e) {
			threw_logic_error = true;
			std::printf("  (caught, as expected: %s)\n", e.what());
		} catch (const std::exception &e) {
			threw_something_else = true;
			std::printf("  (caught WRONG exception type: %s)\n", e.what());
		}
		report(threw_logic_error && !threw_something_else,
		       "ctx_a.submit() with ctx_b's dst buffer throws std::logic_error");
	}

	std::printf("\n%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
