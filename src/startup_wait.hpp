#pragma once

#include <cstddef>
#include <cstdint>

namespace rocos {

// Allocation-free startup gate. Timeout uses elapsed monotonic time, not the
// number of cycles: late wakes must not extend a failed startup indefinitely.
class StartupWait {
public:
    enum class Result { Waiting, Ready, TimedOut };
    explicit StartupWait(std::int64_t start_ns) noexcept
        : deadline_ns_(start_ns + 10000000000LL) {}

    Result observe(std::int64_t now_ns, bool healthy) noexcept {
        if (now_ns >= deadline_ns_) {
            return Result::TimedOut;
        }
        stable_count_ = healthy ? stable_count_ + 1U : 0U;
        return stable_count_ >= 5U ? Result::Ready : Result::Waiting;
    }

private:
    std::int64_t deadline_ns_;
    std::size_t stable_count_{0};
};

} // namespace rocos
