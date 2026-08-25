#pragma once
#include <atomic>
#include <memory>
#include <cassert>
#include <cstdint>

namespace kctl {

// Bounded multi-producer multi-consumer lock-free queue (Vyukov design).
// CellAlignment: 0 (default) means no extra per-slot padding; a nonzero
// value forces alignas(CellAlignment) on each Cell for benchmarking the
// effect of eliminating false sharing between adjacent slots.
template <typename T, size_t CellAlignment = 0>
class MpmcBoundedQueue {
    struct CellBase {
        std::atomic<size_t> sequence;
        T data;
    };
    static constexpr size_t kAlign = CellAlignment == 0 ? alignof(CellBase) : CellAlignment;
    struct alignas(kAlign) Cell : CellBase {};

public:
    explicit MpmcBoundedQueue(size_t capacity_pow2)
        : capacity_(capacity_pow2), mask_(capacity_pow2 - 1) {
        assert(capacity_pow2 > 0 && (capacity_pow2 & (capacity_pow2 - 1)) == 0 &&
               "capacity must be a power of two");
        // std::atomic<size_t> is neither copyable nor movable, so a
        // std::vector<Cell> is not an option -- Vyukov's original design
        // uses raw storage + default construction instead.
        buffer_ = std::make_unique<Cell[]>(capacity_);
        for (size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    bool try_push(T item) {
        Cell* cell;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // queue full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::move(item);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        Cell* cell;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            size_t seq = cell->sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false; // queue empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = std::move(cell->data);
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

private:
    size_t capacity_, mask_;
    std::unique_ptr<Cell[]> buffer_;
    alignas(64) std::atomic<size_t> enqueue_pos_{0};
    alignas(64) std::atomic<size_t> dequeue_pos_{0};
};

} // namespace kctl
