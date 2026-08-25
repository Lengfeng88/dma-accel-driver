#pragma once
#include "kctl/device_handle.hpp"
#include <unordered_set>

namespace kctl::testing {

class MockDeviceBackend : public IDeviceBackend {
public:
    int open(const std::string&, int) override {
        if (fail_open_) return -1;
        int fd = next_fd_++;
        open_fds_.insert(fd);
        ++open_calls_;
        return fd;
    }

    int close(int fd) override {
        open_fds_.erase(fd);
        ++close_calls_;
        return 0;
    }

    bool is_valid_fd(int fd) const override { return fd >= 0; }

    void set_fail_open(bool fail) { fail_open_ = fail; }

    int open_calls() const { return open_calls_; }
    int close_calls() const { return close_calls_; }
    bool is_open(int fd) const { return open_fds_.count(fd) > 0; }

private:
    int next_fd_ = 3;
    int open_calls_ = 0;
    int close_calls_ = 0;
    bool fail_open_ = false;
    std::unordered_set<int> open_fds_;
};

} // namespace kctl::testing
