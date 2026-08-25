#pragma once
#include "kctl/ring_buffer_consumer.hpp"
#include <atomic>
#include <thread>
#include <chrono>

namespace kctl::testing {

class MockRingBufferPollSource : public kctl::IRingBufferPollSource<int> {
public:
    MockRingBufferPollSource(int total_events, int events_per_call)
        : total_events_(total_events), events_per_call_(events_per_call) {}

    int poll(int /*timeout_ms*/, std::function<void(int)> on_event) override {
        int emitted_this_call = 0;
        while (emitted_this_call < events_per_call_ &&
               next_.load(std::memory_order_relaxed) < total_events_) {
            int v = next_.fetch_add(1, std::memory_order_relaxed);
            if (v < total_events_) {
                on_event(v);
                ++emitted_this_call;
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        return emitted_this_call;
    }

    bool drained() const { return next_.load(std::memory_order_relaxed) >= total_events_; }

private:
    int total_events_;
    int events_per_call_;
    std::atomic<int> next_{0};
};

} // namespace kctl::testing
