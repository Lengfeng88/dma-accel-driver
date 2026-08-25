#pragma once
#include <concepts>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <utility>

namespace kctl {

// Structural constraint, not inheritance -- any type with these three
// members satisfies this concept, no base class required.
template <typename T>
concept DeviceTaskLike = requires(T& t) {
    { t.task_id() } -> std::convertible_to<uint64_t>;
    { t.submit() };
    { t.poll_complete() } -> std::convertible_to<bool>;
};

// Type-erased task handle. No virtual anywhere: dispatch goes through a
// hand-written function-pointer vtable instead of a vtable the compiler
// generates for you. Moving a TaskHandle only moves the pointer -- the
// underlying task object never relocates.
class TaskHandle {
public:
    template <DeviceTaskLike T>
    explicit TaskHandle(T task)
        : obj_(new T(std::move(task))), vtable_(&vtable_for<T>) {}

    TaskHandle(TaskHandle&& other) noexcept
        : obj_(other.obj_), vtable_(other.vtable_) {
        other.obj_ = nullptr;
    }
    TaskHandle& operator=(TaskHandle&& other) noexcept {
        if (this != &other) {
            if (obj_) vtable_->destroy(obj_);
            obj_ = other.obj_;
            vtable_ = other.vtable_;
            other.obj_ = nullptr;
        }
        return *this;
    }
    TaskHandle(const TaskHandle&) = delete;
    TaskHandle& operator=(const TaskHandle&) = delete;

    ~TaskHandle() { if (obj_) vtable_->destroy(obj_); }

    uint64_t task_id() const { return vtable_->task_id(obj_); }
    void submit() { vtable_->submit(obj_); }
    bool poll_complete() { return vtable_->poll_complete(obj_); }

private:
    struct VTable {
        uint64_t (*task_id)(void*);
        void (*submit)(void*);
        bool (*poll_complete)(void*);
        void (*destroy)(void*);
    };

    template <typename T>
    static constexpr VTable vtable_for = VTable{
        .task_id = [](void* p) -> uint64_t { return static_cast<T*>(p)->task_id(); },
        .submit = [](void* p) { static_cast<T*>(p)->submit(); },
        .poll_complete = [](void* p) -> bool { return static_cast<T*>(p)->poll_complete(); },
        .destroy = [](void* p) { delete static_cast<T*>(p); },
    };

    void* obj_;
    const VTable* vtable_;
};

class TaskScheduler {
public:
    void enqueue(TaskHandle task) {
        uint64_t id = task.task_id();
        task.submit();
        inflight_.emplace(id, std::move(task));
    }

    std::vector<uint64_t> poll_once() {
        std::vector<uint64_t> done;
        for (auto it = inflight_.begin(); it != inflight_.end();) {
            if (it->second.poll_complete()) {
                done.push_back(it->first);
                it = inflight_.erase(it);
            } else {
                ++it;
            }
        }
        return done;
    }

    size_t inflight_count() const { return inflight_.size(); }
    bool is_inflight(uint64_t id) const { return inflight_.count(id) > 0; }

private:
    std::unordered_map<uint64_t, TaskHandle> inflight_;
};

} // namespace kctl
