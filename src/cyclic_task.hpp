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

/**
 * @brief Running statistics of the real-time cyclic task.
 */
struct CycleStatistics {
    std::uint64_t cycles{0};            ///< Total completed cycles.
    std::uint64_t missed_deadlines{0};  ///< Deadlines skipped without catch-up.
    double minimum_us{0.0};             ///< Shortest observed cycle duration [us].
    double maximum_us{0.0};             ///< Longest observed cycle duration [us].
    double average_us{0.0};             ///< Running average cycle duration [us].
    double current_us{0.0};             ///< Most recent cycle duration [us].
};

/**
 * @brief Copies one cycle of process data between the EtherCAT domains and shared memory.
 *
 * Input flows from the input domain into `shared_input`; output flows from
 * `shared_output` into the output domain. Null or zero-sized buffers are skipped.
 *
 * @param domain_input  Base of the EtherCAT input domain.
 * @param input_size    Input region size in bytes.
 * @param shared_input  Destination `pd_input` mapping.
 * @param shared_output Source `pd_output` mapping.
 * @param domain_output Base of the EtherCAT output domain.
 * @param output_size   Output region size in bytes.
 */
void copyProcessData(const std::uint8_t *domain_input,
                     std::size_t input_size,
                     void *shared_input,
                     const void *shared_output,
                     std::uint8_t *domain_output,
                     std::size_t output_size) noexcept;

/**
 * @brief Advances an absolute deadline by one period, skipping missed cycles.
 *
 * If the current time is already past one or more periods, the returned deadline
 * is the first strictly-future deadline and `missed_intervals` reports how many
 * periods were skipped; historical cycles are never re-run.
 *
 * @param previous_deadline   The last absolute deadline.
 * @param period_us           Cycle period in microseconds.
 * @param now                 Current monotonic time.
 * @param[out] missed_intervals Number of skipped periods (0 on normal cadence).
 * @return The next absolute deadline.
 */
timespec advanceDeadline(const timespec &previous_deadline,
                         std::uint32_t period_us,
                         const timespec &now,
                         std::uint64_t &missed_intervals) noexcept;

/**
 * @brief Publishes one cycle's state and timing into the shared bus snapshot.
 *
 * @param state                  Bus/domain state snapshot for this cycle.
 * @param statistics             Running cycle statistics (updated in place).
 * @param period_us              Configured cycle period in microseconds.
 * @param monotonic_timestamp_us Monotonic wake timestamp in microseconds.
 * @param bus                    Shared EcatBus to update.
 */
void updateSharedBus(const BusState &state,
                     CycleStatistics &statistics,
                     std::uint32_t period_us,
                     long monotonic_timestamp_us,
                     EcatBus &bus) noexcept;

/**
 * @brief Absolute-time real-time cyclic task driving one EtherCAT master.
 *
 * Owns no resources; it references an initialized EthercatMaster and a mapped
 * SharedMemoryConfig and runs until the caller clears the stop flag.
 */
class CyclicTask {
public:
    /**
     * @brief Constructs a cyclic task bound to an initialized master and IPC.
     * @param master    Initialized EtherCAT master.
     * @param ipc       Mapped shared-memory configuration.
     * @param period_us Cycle period in microseconds (must be >= 1000).
     */
    CyclicTask(EthercatMaster &master, SharedMemoryConfig &ipc, std::uint32_t period_us) noexcept;

    /**
     * @brief Runs the cyclic loop until @p stop_requested is set.
     * @param stop_requested Async-signal-safe flag set by the signal handler.
     * @return 0 on clean stop, otherwise an errno-style error code.
     */
    int run(volatile std::sig_atomic_t &stop_requested) noexcept;

    /**
     * @brief Returns the statistics accumulated during run().
     */
    const CycleStatistics &statistics() const noexcept;

private:
    EthercatMaster &master_;
    SharedMemoryConfig &ipc_;
    std::uint32_t period_us_;
    CycleStatistics statistics_{};
};

}  // namespace rocos
