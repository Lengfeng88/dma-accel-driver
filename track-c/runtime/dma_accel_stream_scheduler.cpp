// dma_accel_stream_scheduler.cpp — see dma_accel_stream_scheduler.hpp for
// the design rationale, in particular why this stays single-threaded.

#include "dma_accel_stream_scheduler.hpp"

#include <stdexcept>

namespace dma_accel {

std::size_t StreamScheduler::add_stream(Stream &stream) {
	streams_.push_back(&stream);
	queues_.emplace_back();
	return streams_.size() - 1;
}

void StreamScheduler::enqueue(std::size_t stream_index, std::function<Fence()> submit_fn) {
	if (stream_index >= streams_.size()) {
		throw std::out_of_range("StreamScheduler::enqueue(): stream_index out of range");
	}
	queues_[stream_index].push_back(std::move(submit_fn));
}

std::vector<ScheduledResult> StreamScheduler::run() {
	std::vector<ScheduledResult> results;

	// One round-robin pass per outer iteration: try to submit one
	// command from every registered stream's queue, in registration
	// order. Stop once a full pass submits nothing at all — meaning
	// every queue is empty.
	for (;;) {
		bool progressed = false;
		for (std::size_t i = 0; i < queues_.size(); ++i) {
			if (!queues_[i].empty()) {
				auto fn = std::move(queues_[i].front());
				queues_[i].pop_front();
				Fence f = fn();
				results.push_back(ScheduledResult{i, std::move(f)});
				progressed = true;
			}
		}
		if (!progressed) {
			break;
		}
	}

	return results;
}

} // namespace dma_accel
