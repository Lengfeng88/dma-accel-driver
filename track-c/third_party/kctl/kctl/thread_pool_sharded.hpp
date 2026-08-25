#pragma once
#include "kctl/ring_buffer_lockfree.hpp"
#include <thread>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>

namespace kctl {

// Zero cross-worker contention: each worker owns a private SPSC queue.
// IMPORTANT: submit() is NOT safe to call from multiple threads
// concurrently -- each shard queue is SPSC.
class ThreadPoolSharded {
public:
    ThreadPoolSharded(size_t num_workers, size_t per_shard_capacity_pow2) {
        shards_.reserve(num_workers);
        for (size_t i = 0; i < num_workers; ++i) {
            shards_.push_back(std::make_unique<RingBufferLockfree<std::function<void()>>>(
                per_shard_capacity_pow2));
        }
        workers_.reserve(num_workers);
        for (size_t i = 0; i < num_workers; ++i) {
            workers_.emplace_back([this, i] { worker_loop(i); });
        }
    }

    ~ThreadPoolSharded() {
        stopping_.store(true, std::memory_order_relaxed);
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

    ThreadPoolSharded(const ThreadPoolSharded&) = delete;
    ThreadPoolSharded& operator=(const ThreadPoolSharded&) = delete;

    // Single-threaded caller only -- see class comment.
    bool submit(std::function<void()> task) {
        size_t shard = next_shard_++ % shards_.size();
        return shards_[shard]->try_push(std::move(task));
    }

    size_t completed_count() const { return completed_.load(std::memory_order_relaxed); }

private:
    void worker_loop(size_t shard_idx) {
        auto& shard = *shards_[shard_idx];
        while (!stopping_.load(std::memory_order_relaxed)) {
            if (auto task = shard.try_pop()) {
                (*task)();
                completed_.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
        while (auto task = shard.try_pop()) {
            (*task)();
            completed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::vector<std::unique_ptr<RingBufferLockfree<std::function<void()>>>> shards_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> completed_{0};
    size_t next_shard_ = 0;
};

} // namespace kctl
