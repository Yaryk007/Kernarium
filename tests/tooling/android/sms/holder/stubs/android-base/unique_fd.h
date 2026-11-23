// SPDX-License-Identifier: GPL-2.0-or-later
// A stand-in for android-base/unique_fd.h (see prelude.h).
#pragma once
#include <unistd.h>

namespace android {
namespace base {
class unique_fd {
  public:
    unique_fd() = default;
    explicit unique_fd(int fd) : fd_(fd) {}
    unique_fd(unique_fd&& other) noexcept : fd_(other.release()) {}
    unique_fd& operator=(unique_fd&& other) noexcept {
        reset(other.release());
        return *this;
    }
    ~unique_fd() { reset(); }
    int get() const { return fd_; }
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) {
        if (fd_ >= 0) close(fd_);
        fd_ = fd;
    }

  private:
    int fd_ = -1;
};
}  // namespace base
}  // namespace android
