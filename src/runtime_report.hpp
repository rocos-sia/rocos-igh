#pragma once

#include <atomic>
#include <cstdint>

namespace rocos {

struct TimingSummary {
    std::uint64_t samples{0};
    double minimum_us{0}, average_us{0}, maximum_us{0};

    void add(double us) noexcept {
        if (samples == 0 || us < minimum_us) minimum_us = us;
        if (samples == 0 || us > maximum_us) maximum_us = us;
        ++samples;
        average_us += (us - average_us) / static_cast<double>(samples);
    }
};

// Process-local diagnostics only; never part of the shared-memory ABI.
struct RuntimeReport {
    double window_seconds{0};
    unsigned int responding_slaves{0}, al_states{0};
    unsigned int input_wc{0}, output_wc{0};
    unsigned int input_wc_state{0}, output_wc_state{0};
    bool link_up{false}, authorized{false};
    TimingSummary cycle, task, receive, pdo, send;
    std::uint64_t missed{0}, wc_errors{0}, link_down{0};
    std::uint64_t total_missed{0}, total_wc_errors{0}, total_link_down{0};
};

// Single producer (cyclic thread), single consumer (reporter). Ownership of
// slot_ transfers via release/acquire, so even its non-atomic fields are safe.
// A slow/blocked reporter drops reports, never delays the cyclic thread.
class RuntimeReportMailbox {
public:
    bool publish(const RuntimeReport &report) noexcept {
        if (ready_.load(std::memory_order_acquire)) return false;
        slot_ = report;
        ready_.store(true, std::memory_order_release);
        return true;
    }
    bool consume(RuntimeReport &report) noexcept {
        if (!ready_.load(std::memory_order_acquire)) return false;
        report = slot_;
        ready_.store(false, std::memory_order_release);
        return true;
    }
private:
    static_assert(std::atomic<bool>::is_always_lock_free);
    RuntimeReport slot_{};
    std::atomic<bool> ready_{false};
};

} // namespace rocos
