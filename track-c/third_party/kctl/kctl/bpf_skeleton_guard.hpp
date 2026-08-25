#pragma once
#include <stdexcept>

namespace kctl {

template <typename Skel>
class IBpfBackend {
public:
    virtual ~IBpfBackend() = default;
    virtual Skel* open() = 0;
    virtual int load(Skel*) = 0;
    virtual int attach(Skel*) = 0;
    virtual void detach(Skel*) = 0;
    virtual void destroy(Skel*) = 0;
};

template <typename Skel>
class BpfSkeletonGuard {
public:
    explicit BpfSkeletonGuard(IBpfBackend<Skel>& backend) : backend_(backend) {
        skel_ = backend_.open();
        if (!skel_) {
            throw std::runtime_error("bpf: open failed");
        }

        if (backend_.load(skel_) != 0) {
            backend_.destroy(skel_);
            skel_ = nullptr;
            throw std::runtime_error("bpf: load failed");
        }

        if (backend_.attach(skel_) != 0) {
            backend_.destroy(skel_);
            skel_ = nullptr;
            throw std::runtime_error("bpf: attach failed");
        }

        attached_ = true;
    }

    ~BpfSkeletonGuard() {
        if (skel_) {
            if (attached_) backend_.detach(skel_);
            backend_.destroy(skel_);
        }
    }

    BpfSkeletonGuard(const BpfSkeletonGuard&) = delete;
    BpfSkeletonGuard& operator=(const BpfSkeletonGuard&) = delete;

    Skel* get() const { return skel_; }

private:
    IBpfBackend<Skel>& backend_;
    Skel* skel_ = nullptr;
    bool attached_ = false;
};

} // namespace kctl
