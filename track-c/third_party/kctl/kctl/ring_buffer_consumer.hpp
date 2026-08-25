#pragma once
#include "kctl/ring_buffer_lockfree.hpp"
#include <thread>
#include <atomic>
#include <functional>
#include <optional>

namespace kctl {

// A real backend wraps libbpf's ring_buffer__poll(), which must always be
// called from the same single thread.
template <typename Event>
class IRingBufferPollSource {
public:
    virtual ~IRingBufferPollSource() = default;
    virtual int poll(int timeout_ms, std::function<void(Event)> on_event) = 0;
};

template <typename Event>
class RingBufferConsumer {
public:
    RingBufferConsumer(IRingBufferPollSource<Event>& source, size_t queue_capacity_pow2)
        : source_(source), queue_(queue_capacity_pow2) {}

    ~RingBufferConsumer() { stop(); }

    void start() {
        running_ = true;
        poll_thread_ = std::thread([this] { poll_loop(); });
    }

    void stop() {
        if (running_) {
            running_ = false;
            if (poll_thread_.joinable()) poll_thread_.join();
        }
    }

    std::optional<Event> try_consume() { return queue_.try_pop(); }

    size_t dropped_count() const { return dropped_.load(std::memory_order_relaxed); }

private:
    void poll_loop() {
        while (running_) {
            source_.poll(10, [this](Event ev) {
                // drop-on-full: never block the poll thread, even at the
                // cost of losing events.
                if (!queue_.try_push(std::move(ev))) {
                    dropped_.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
    }

    IRingBufferPollSource<Event>& source_;
    RingBufferLockfree<Event> queue_;
    std::thread poll_thread_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> dropped_{0};
};

} // namespace kctl
