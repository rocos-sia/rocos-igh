#pragma once

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <ctime>

namespace rocos {

struct BusState;
struct EcatBus;
class EthercatMaster;
class SharedMemoryConfig;

struct CycleStatistics {
    std::uint64_t cycles{0};
    std::uint64_t missed_deadlines{0};
    double minimum_us{0.0};
    double maximum_us{0.0};
    double average_us{0.0};
    double current_us{0.0};
};

void copyProcessData(const std::uint8_t *domain_input,
                     std::size_t input_size,
                     void *shared_input,
                     const void *shared_output,
                     std::uint8_t *domain_output,
                     std::size_t output_size) noexcept;

timespec advanceDeadline(const timespec &previous_deadline,
                         std::uint32_t period_us,
                         const timespec &now,
                         std::uint64_t &missed_intervals) noexcept;

void updateSharedBus(const BusState &state,
                     CycleStatistics &statistics,
                     std::uint32_t period_us,
                     long monotonic_timestamp_us,
                     EcatBus &bus) noexcept;

class CyclicTask {
public:
    CyclicTask(EthercatMaster &master, SharedMemoryConfig &ipc, std::uint32_t period_us) noexcept;
    int run(volatile std::sig_atomic_t &stop_requested) noexcept;
    const CycleStatistics &statistics() const noexcept;

private:
    EthercatMaster &master_;
    SharedMemoryConfig &ipc_;
    std::uint32_t period_us_;
    CycleStatistics statistics_{};
};

}  // namespace rocos
