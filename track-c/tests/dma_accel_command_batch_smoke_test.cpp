// dma_accel_command_batch_smoke_test.cpp
//
// C4 smoke test:
//   1. Happy path: build -> freeze -> submit, verify the returned
//      CommandGroup is AllOk and every command's data is correct.
//   2. append() after freeze() throws.
//   3. submit() before freeze() throws.
//   4. submit() called twice throws (no double-submission).
//   5. A mid-batch ioctl-level failure (out-of-bounds offset+len) makes
//      submit() throw, and demonstrates — deliberately, not as a bug —
//      the documented KNOWN, UNRECOVERED gap: the command(s) before the
//      failure were already dispatched to hardware, but this API gives
//      the caller no way to retrieve their Fences afterward. This test
//      exists to prove the documented limitation is real and understood,
//      not to claim it's acceptable to leave silently unverified.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -o dma_accel_command_batch_smoke_test
//     dma_accel_command_batch_smoke_test.cpp dma_accel_command_batch.cpp
//     dma_accel_command_group.cpp dma_accel_runtime.cpp
// Run:
//   sudo ./dma_accel_command_batch_smoke_test [/dev/dma_accel0]

#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "dma_accel_command_batch.hpp"
#include "dma_accel_command_group.hpp"
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
constexpr int kBatchSize = 4;

bool test_happy_path(const char *device) {
	std::printf("-- 1. happy path: build -> freeze -> submit --\n");
	dma_accel::Stream stream(device);

	std::vector<dma_accel::Buffer> srcs;
	std::vector<dma_accel::Buffer> dsts;
	srcs.reserve(kBatchSize);
	dsts.reserve(kBatchSize);
	for (int i = 0; i < kBatchSize; ++i) {
		srcs.push_back(stream.alloc_buffer(kBufSize));
		dsts.push_back(stream.alloc_buffer(kBufSize));
		for (std::uint32_t b = 0; b < kBufSize; ++b) {
			srcs[i].data()[b] = static_cast<std::uint8_t>((b * 17 + 5 + i * 23) & 0xFF);
		}
		std::memset(dsts[i].data(), 0, kBufSize);
	}

	dma_accel::CommandBatch batch(stream);
	for (int i = 0; i < kBatchSize; ++i) {
		batch.append(OPCODE_COPY, srcs[i], 0, dsts[i], 0, kBufSize);
	}
	std::printf("appended %zu commands, frozen=%d\n", batch.size(), batch.is_frozen());

	batch.freeze();
	dma_accel::CommandGroup group = batch.submit();

	dma_accel::GroupStatus status = group.wait(2000);
	if (status != dma_accel::GroupStatus::AllOk) {
		std::fprintf(stderr, "FAIL: batch group.wait() != AllOk\n");
		return false;
	}
	for (int i = 0; i < kBatchSize; ++i) {
		if (std::memcmp(srcs[i].data(), dsts[i].data(), kBufSize) != 0) {
			std::fprintf(stderr, "FAIL: pair %d mismatch after batched COPY\n", i);
			return false;
		}
	}
	std::printf("PASS: all %d batched commands completed AllOk, data verified correct\n", kBatchSize);
	return true;
}

bool test_lifecycle_guards(const char *device) {
	std::printf("-- 2/3/4. lifecycle guards --\n");
	dma_accel::Stream stream(device);
	bool ok = true;

	// append() after freeze() throws.
	{
		dma_accel::Buffer src = stream.alloc_buffer(kBufSize);
		dma_accel::Buffer dst = stream.alloc_buffer(kBufSize);
		dma_accel::CommandBatch batch(stream);
		batch.freeze();
		bool threw = false;
		try {
			batch.append(OPCODE_COPY, src, 0, dst, 0, kBufSize);
		} catch (const std::logic_error &) {
			threw = true;
		}
		if (!threw) {
			std::fprintf(stderr, "FAIL: append() after freeze() did not throw\n");
			ok = false;
		} else {
			std::printf("PASS: append() after freeze() threw std::logic_error\n");
		}
	}

	// submit() before freeze() throws.
	{
		dma_accel::Buffer src = stream.alloc_buffer(kBufSize);
		dma_accel::Buffer dst = stream.alloc_buffer(kBufSize);
		dma_accel::CommandBatch batch(stream);
		batch.append(OPCODE_COPY, src, 0, dst, 0, kBufSize);
		bool threw = false;
		try {
			dma_accel::CommandGroup g = batch.submit();
			(void)g;
		} catch (const std::logic_error &) {
			threw = true;
		}
		if (!threw) {
			std::fprintf(stderr, "FAIL: submit() before freeze() did not throw\n");
			ok = false;
		} else {
			std::printf("PASS: submit() before freeze() threw std::logic_error\n");
		}
	}

	// submit() called twice throws.
	{
		dma_accel::Buffer src = stream.alloc_buffer(kBufSize);
		dma_accel::Buffer dst = stream.alloc_buffer(kBufSize);
		dma_accel::CommandBatch batch(stream);
		batch.append(OPCODE_COPY, src, 0, dst, 0, kBufSize);
		batch.freeze();
		dma_accel::CommandGroup g1 = batch.submit();
		g1.wait(2000); // drain before trying again, tidy but not required for the check itself

		bool threw = false;
		try {
			dma_accel::CommandGroup g2 = batch.submit();
			(void)g2;
		} catch (const std::logic_error &) {
			threw = true;
		}
		if (!threw) {
			std::fprintf(stderr, "FAIL: second submit() call did not throw\n");
			ok = false;
		} else {
			std::printf("PASS: second submit() call threw std::logic_error (no double-submission)\n");
		}
	}

	return ok;
}

bool test_mid_batch_failure(const char *device) {
	std::printf("-- 5. mid-batch ioctl failure (documented, unrecovered gap) --\n");
	dma_accel::Stream stream(device);

	dma_accel::Buffer src_ok = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer dst_ok = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer src_bad = stream.alloc_buffer(kBufSize);
	dma_accel::Buffer dst_bad = stream.alloc_buffer(kBufSize);
	std::memset(src_ok.data(), 0x5A, kBufSize);
	std::memset(dst_ok.data(), 0, kBufSize);

	dma_accel::CommandBatch batch(stream);
	// Command 0: valid — will actually reach hardware before the failure.
	batch.append(OPCODE_COPY, src_ok, 0, dst_ok, 0, kBufSize);
	// Command 1: invalid — offset + len goes past the buffer's actual
	// allocated size. Per Stream::submit()'s own doc comment in
	// dma_accel_runtime.hpp, this is expected to throw std::system_error
	// at ioctl time (not something that silently succeeds and fails
	// later, unlike the bad-SCALE_ADD-length case used in the C3 test).
	constexpr std::uint32_t kBadOffset = kBufSize - 8;
	constexpr std::uint32_t kBadLen = kBufSize; // offset + len far exceeds buffer size
	batch.append(OPCODE_COPY, src_bad, kBadOffset, dst_bad, 0, kBadLen);

	batch.freeze();

	bool threw = false;
	try {
		dma_accel::CommandGroup g = batch.submit();
		(void)g;
	} catch (const std::system_error &) {
		threw = true;
	}
	if (!threw) {
		std::fprintf(stderr, "FAIL: expected std::system_error from the out-of-bounds command, "
				      "batch.submit() did not throw\n");
		return false;
	}
	std::printf("confirmed submit() threw on the invalid command, as documented\n");

	// The batch now correctly refuses to retry (submitted_ was set
	// before the loop) — verifying this doesn't accidentally re-attempt
	// and double-submit command 0.
	bool second_threw = false;
	try {
		dma_accel::CommandGroup g2 = batch.submit();
		(void)g2;
	} catch (const std::logic_error &) {
		second_threw = true;
	}
	if (!second_threw) {
		std::fprintf(stderr, "FAIL: a second submit() after the mid-batch failure did not "
				      "throw — risk of double-submission\n");
		return false;
	}

	std::printf("PASS: mid-batch failure behaved exactly as documented — submit() threw, "
		    "no retry/double-submission possible, and command 0's Fence is (by design, "
		    "not by bug) unrecoverable through this API\n");
	return true;
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel C4 CommandBatch smoke test ==\n");
	std::printf("using device %s (each scenario opens its own Stream/session)\n", device);

	try {
		bool ok = true;
		ok = test_happy_path(device) && ok;
		ok = test_lifecycle_guards(device) && ok;
		ok = test_mid_batch_failure(device) && ok;

		if (!ok) {
			std::fprintf(stderr, "FAIL: one or more checks failed, see above\n");
			return 1;
		}

		std::printf("PASS: CommandBatch verified — build/freeze/submit, lifecycle guards, "
			    "and the documented mid-batch-failure behavior all correct\n");
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
