#pragma once
#include <string>
#include <system_error>
#include <cerrno>

namespace kctl {

class IDeviceBackend {
public:
    virtual ~IDeviceBackend() = default;
    virtual int open(const std::string& path, int flags) = 0;
    virtual int close(int fd) = 0;
    virtual bool is_valid_fd(int fd) const = 0;
};

class DeviceHandle {
public:
    DeviceHandle(IDeviceBackend& backend, const std::string& path, int flags)
        : backend_(backend) {
        fd_ = backend_.open(path, flags);
        if (!backend_.is_valid_fd(fd_)) {
            throw std::system_error(errno, std::generic_category(),
                                     "DeviceHandle: open failed for " + path);
        }
    }

    ~DeviceHandle() {
        if (backend_.is_valid_fd(fd_)) backend_.close(fd_);
    }

    DeviceHandle(const DeviceHandle&) = delete;
    DeviceHandle& operator=(const DeviceHandle&) = delete;

    DeviceHandle(DeviceHandle&& other) noexcept
        : backend_(other.backend_), fd_(other.fd_) {
        other.fd_ = -1;
    }

    DeviceHandle& operator=(DeviceHandle&& other) noexcept {
        if (this != &other) {
            if (backend_.is_valid_fd(fd_)) backend_.close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int fd() const { return fd_; }

private:
    IDeviceBackend& backend_;
    int fd_ = -1;
};

} // namespace kctl
