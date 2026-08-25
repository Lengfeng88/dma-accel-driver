#pragma once
#include <atomic>
#include <vector>
#include <optional>
#include <cassert>

namespace kctl {

// Single-Producer Single-Consumer lock-free ring buffer.
// try_push() must only be called from the producer thread.
// try_pop()  must only be called from the consumer thread.
template <typename T>
class RingBufferLockfree {
public:
    explicit RingBufferLockfree(size_t capacity_pow2)
        : mask_(capacity_pow2 - 1), buf_(capacity_pow2) {
        assert(capacity_pow2 > 0 && (capacity_pow2 & (capacity_pow2 - 1)) == 0 &&
               "capacity must be a power of two");
    }

    bool try_push(T item) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        if (tail - head == buf_.size()) return false; // full
        buf_[tail & mask_] = std::move(item);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::optional<T> try_pop() {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail) return std::nullopt; // empty
        T item = std::move(buf_[head & mask_]);
        head_.store(head + 1, std::memory_order_release);
        return item;
    }

private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    size_t mask_;
    std::vector<T> buf_;
};

} // namespace kctl
