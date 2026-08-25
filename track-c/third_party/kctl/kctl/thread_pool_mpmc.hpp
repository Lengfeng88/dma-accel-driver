#pragma once
#include "kctl/mpmc_bounded_queue.hpp"
#include <thread>
#include <vector>
#include <functional>
#include <atomic>

namespace kctl {

class ThreadPoolMpmc {
public:
    ThreadPoolMpmc(size_t num_workers, size_t queue_capacity_pow2)
        : queue_(queue_capacity_pow2) {
        workers_.reserve(num_workers);
        for (size_t i = 0; i < num_workers; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPoolMpmc() {
        stopping_.store(true, std::memory_order_relaxed);
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

    ThreadPoolMpmc(const ThreadPoolMpmc&) = delete;
    ThreadPoolMpmc& operator=(const ThreadPoolMpmc&) = delete;

    // Safe to call concurrently from any number of threads.
    bool submit(std::function<void()> task) {
        return queue_.try_push(std::move(task));
    }

    size_t completed_count() const { return completed_.load(std::memory_order_relaxed); }

private:
    void worker_loop() {
        while (!stopping_.load(std::memory_order_relaxed)) {
            std::function<void()> task;
            if (queue_.try_pop(task)) {
                task();
                completed_.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
        // Drain remaining tasks so a stop() during heavy load doesn't
        // silently discard work that was already accepted.
        std::function<void()> task;
        while (queue_.try_pop(task)) {
            task();
            completed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    MpmcBoundedQueue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> completed_{0};
};

} // namespace kctl
