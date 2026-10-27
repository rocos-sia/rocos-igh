#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <rocos_shm/access.hpp>

namespace rocos {
// One non-RT command worker submits; the cyclic thread acknowledges only after
// resetting and publishing statistics. A timeout never cancels a pending reset.
class StatisticsReset {
public:
    std::int32_t execute(std::uint64_t deadline, const std::atomic<bool> &stop) noexcept {
        if (stop.load()) return STATE_MISMATCH;
        if (monotonicNs() >= deadline) return DEADLINE_EXCEEDED;
        const auto previous = requested_.load();
        if (acknowledged_.load() != previous) return BUSY;
        const auto ticket = previous + 1;
        requested_.store(ticket);
        while (acknowledged_.load() != ticket) {
            if (stop.load() || monotonicNs() >= deadline) return OUTCOME_UNKNOWN;
            const timespec pause{0, 1000000};
            nanosleep(&pause, nullptr);
        }
        return RESULT_OK;
    }

    std::uint64_t pending() const noexcept {
        const auto ticket = requested_.load();
        return ticket != acknowledged_.load() ? ticket : 0;
    }
    void acknowledge(std::uint64_t ticket) noexcept { acknowledged_.store(ticket); }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> requested_{0}, acknowledged_{0};
};
} // namespace rocos
