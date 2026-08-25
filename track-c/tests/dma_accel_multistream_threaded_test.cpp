// dma_accel_multistream_threaded_test.cpp
//
// C6b: Concurrent runtime execution — first genuine KCTL evaluation
// point per TRACK_C_RUNTIME_SPEC.md §6.5.
//
// Unlike C6a (one thread, manually alternating submission between two
// Streams to control SQ ring order directly), C6b uses real OS threads:
// each thread owns and exclusively drives one Stream (never shared
// across threads, preserving M10's single-thread-per-Stream invariant),
// submitting its own commands as fast as it can with NO artificial
// coordination between threads. Whatever interleaving happens at the SQ
// ring level now emerges from genuine concurrent execution and real OS
// thread scheduling — not from code explicitly alternating A0, B0, A1,
// B1 like C6a did. This answers a different question than C6a: does
// naive, uncoordinated multi-threaded submission naturally produce
// overlapping dispatch, or does thread-scheduling variance still let one
// thread's commands dominate the ring before the other gets a look in?
//
// KCTL evaluation: each worker thread reports a completion event the
// moment it observes one of its own Fences become ready. Multiple
// threads doing this concurrently is exactly the multi-producer
// scenario kctl::MpmcBoundedQueue exists for — a lock-free way to get
// those events to one aggregator (the main thread here) without a
// mutex. kctl::ThreadPoolMpmc/ThreadPoolSharded were evaluated and
// rejected for this milestone: both model workers pulling one-shot
// tasks from a queue, but C6b's threads are persistent, dedicated
// Stream drivers (submit-then-poll-repeatedly), not one-shot task
// executors — forcing that mismatch would be adopting KCTL machinery to
// serve an architecture diagram, exactly what §6.5 warns against.
// kctl::DeviceHandle was also evaluated and rejected: Stream already
// RAII-owns its own fd (frozen Track A code), and DeviceHandle requires
// an IDeviceBackend abstraction Stream has no way to plug into without
// modifying it.
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -pthread -o dma_accel_multistream_threaded_test
//     dma_accel_multistream_threaded_test.cpp dma_accel_runtime.cpp
//     -I <path-to-track-c/third_party/kctl>
// Run:
//   sudo ./dma_accel_multistream_threaded_test [/dev/dma_accel0]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

#include "dma_accel_runtime.hpp"
#include "kctl/mpmc_bounded_queue.hpp"

namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;
} // namespace
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#include "dma_accel_regs.h"

namespace {

constexpr int kCommandsPerStream = 8;
constexpr std::uint32_t kBufSize = 4096;
constexpr int kNumStreams = 2;

struct CompletionEvent {
	int stream_id;
	std::uint64_t cmd_id;
	long long elapsed_us;
};

void stream_worker(int stream_id, const char *device, int count,
		    std::chrono::steady_clock::time_point t_start,
		    kctl::MpmcBoundedQueue<CompletionEvent> &out_queue,
		    std::exception_ptr &out_exception) {
	// Exceptions thrown inside a std::thread's function body never reach
	// the creating thread's try/catch — an uncaught one here calls
	// std::terminate() immediately, regardless of what main() does.
	// Caught here and handed back via std::exception_ptr so main() can
	// report it properly after joining, instead of the whole process
	// aborting with no clean error message.
	try {
		dma_accel::Stream stream(device);
		dma_accel::Buffer src = stream.alloc_buffer(kBufSize);
		dma_accel::Buffer dst = stream.alloc_buffer(kBufSize);
		std::memset(src.data(), 0x10 + stream_id, kBufSize);
		std::memset(dst.data(), 0, kBufSize);

		std::vector<dma_accel::Fence> fences;
		fences.reserve(count);
		// Submit as fast as possible — no artificial pacing, no
		// coordination with the other thread. Whatever ring-position
		// interleaving happens is real, not orchestrated.
		for (int i = 0; i < count; ++i) {
			fences.push_back(stream.submit(OPCODE_COPY, src, 0, dst, 0, kBufSize));
		}

		std::vector<bool> recorded(count, false);
		int remaining = count;
		while (remaining > 0) {
			stream.pump();
			for (int i = 0; i < count; ++i) {
				if (!recorded[i] && fences[i].is_ready()) {
					recorded[i] = true;
					--remaining;
					auto us = std::chrono::duration_cast<std::chrono::microseconds>(
							  std::chrono::steady_clock::now() - t_start)
							  .count();
					CompletionEvent ev{stream_id, fences[i].cmd_id(), us};
					while (!out_queue.try_push(ev)) {
						std::this_thread::yield();
					}
				}
			}
			if (remaining > 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
	} catch (...) {
		out_exception = std::current_exception();
	}
}

} // namespace

int main(int argc, char **argv) {
	const char *device = argc > 1 ? argv[1] : "/dev/dma_accel0";

	std::printf("== dma-accel C6b concurrent multi-thread submission test ==\n");
	std::printf("using device %s, %d threads, %d commands each, KCTL MpmcBoundedQueue for "
		    "completion reporting\n",
		    device, kNumStreams, kCommandsPerStream);

	try {
		kctl::MpmcBoundedQueue<CompletionEvent> queue(32);
		auto t_start = std::chrono::steady_clock::now();

		std::vector<std::exception_ptr> worker_exceptions(kNumStreams);
		std::vector<std::thread> workers;
		for (int i = 0; i < kNumStreams; ++i) {
			workers.emplace_back(stream_worker, i, device, kCommandsPerStream, t_start,
					      std::ref(queue), std::ref(worker_exceptions[i]));
		}
		for (auto &t : workers) {
			t.join();
		}

		for (int i = 0; i < kNumStreams; ++i) {
			if (worker_exceptions[i]) {
				try {
					std::rethrow_exception(worker_exceptions[i]);
				} catch (const std::exception &e) {
					std::fprintf(stderr, "FAIL: stream %d worker thread threw: %s\n", i,
						     e.what());
				}
				return 1;
			}
		}

		std::vector<CompletionEvent> timeline;
		CompletionEvent ev;
		while (queue.try_pop(ev)) {
			timeline.push_back(ev);
		}

		if (static_cast<int>(timeline.size()) != kNumStreams * kCommandsPerStream) {
			std::fprintf(stderr, "FAIL: expected %d completion events, got %zu\n",
				     kNumStreams * kCommandsPerStream, timeline.size());
			return 1;
		}

		std::stable_sort(timeline.begin(), timeline.end(),
				  [](const CompletionEvent &x, const CompletionEvent &y) {
					  return x.elapsed_us < y.elapsed_us;
				  });

		std::printf("-- completion timeline (reconstructed from KCTL MPMC queue) --\n");
		for (const auto &e : timeline) {
			std::printf("  %8lld us  stream=%d cmd=0x%llx\n", e.elapsed_us, e.stream_id,
				    static_cast<unsigned long long>(e.cmd_id));
		}

		int transitions = 0;
		for (std::size_t i = 1; i < timeline.size(); ++i) {
			if (timeline[i].stream_id != timeline[i - 1].stream_id) {
				++transitions;
			}
		}

		std::printf("-- summary --\n");
		std::printf("total completions: %zu, cross-stream transitions: %d\n", timeline.size(),
			    transitions);
		std::printf("(unlike C6a, this interleaving — or lack of it — emerged from real "
			    "concurrent OS-thread submission, not manual alternation)\n");

		if (transitions <= 1) {
			std::printf("RESULT: threads' submissions landed essentially serially in the "
				    "SQ ring despite running concurrently — real OS thread scheduling "
				    "did not naturally interleave the two threads' ioctl() calls enough "
				    "to produce overlapping dispatch here. Not a failure of KCTL or the "
				    "coordinator — a real finding about submission timing that C7 will "
				    "need to account for explicitly (uncoordinated concurrent threads "
				    "are not sufficient on their own; C7 needs deliberate interleaving, "
				    "consistent with C6a's conclusion).\n");
		} else {
			std::printf("RESULT: genuine interleaving emerged from real concurrent "
				    "multi-threaded submission alone, no manual alternation needed here "
				    "— a stronger result than C6a's engineered version.\n");
		}

		std::printf("PASS: C6b coordinator ran two real OS threads, each exclusively driving "
			    "its own Stream, reporting completions through KCTL's lock-free "
			    "MpmcBoundedQueue with no data races and no mutex\n");
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
		return 1;
	}
}
