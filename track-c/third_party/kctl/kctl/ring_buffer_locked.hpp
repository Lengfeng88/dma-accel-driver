#pragma once
#include <mutex>
#include <vector>
#include <optional>

namespace kctl {

template <typename T>
class RingBufferLocked {
public:
    explicit RingBufferLocked(size_t capacity) : buf_(capacity) {}

    bool try_push(T item) {
        std::lock_guard<std::mutex> lk(mu_);
        if (count_ == buf_.size()) return false;
        buf_[tail_] = std::move(item);
        tail_ = (tail_ + 1) % buf_.size();
        ++count_;
        return true;
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lk(mu_);
        if (count_ == 0) return std::nullopt;
        T item = std::move(buf_[head_]);
        head_ = (head_ + 1) % buf_.size();
        --count_;
        return item;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return count_;
    }

private:
    std::vector<T> buf_;
    size_t head_ = 0, tail_ = 0, count_ = 0;
    mutable std::mutex mu_;
};

} // namespace kctl
