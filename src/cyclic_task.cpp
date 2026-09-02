#include "cyclic_task.hpp"

#include <cerrno>
#include <cstring>

#include "shared_memory_config.hpp"

#if ROCOS_IGH_BUILD_MASTER
#include "ethercat_master.hpp"
#endif

namespace rocos {
namespace {

constexpr std::int64_t kNsecPerSec = 1000000000LL;
constexpr std::uint32_t kMinPeriodUs = 1000U;

// Converts a timespec to a single signed nanosecond count.
std::int64_t toNanoseconds(const timespec &ts) noexcept {
    return static_cast<std::int64_t>(ts.tv_sec) * kNsecPerSec + static_cast<std::int64_t>(ts.tv_nsec);
}

// Converts a signed nanosecond count back to a normalized timespec.
timespec fromNanoseconds(std::int64_t nanoseconds) noexcept {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(nanoseconds / kNsecPerSec);
    ts.tv_nsec = static_cast<long>(nanoseconds % kNsecPerSec);
    if (ts.tv_nsec < 0) {
        ts.tv_nsec += static_cast<long>(kNsecPerSec);
        --ts.tv_sec;
    }
    return ts;
}

// Returns the timespec truncated to whole microseconds.
long toMicroseconds(const timespec &ts) noexcept {
    return static_cast<long>(toNanoseconds(ts) / 1000LL);
}

// Clears the cycle count and timing aggregates, leaving missed_deadlines untouched.
void resetTimingStatistics(CycleStatistics &statistics) noexcept {
    statistics.cycles = 0;
    statistics.minimum_us = 0.0;
    statistics.maximum_us = 0.0;
    statistics.average_us = 0.0;
    statistics.current_us = 0.0;
}

// Folds one measured cycle duration into the running min/max/average/current stats.
void accountCycleDuration(CycleStatistics &statistics, double duration_us) noexcept {
    statistics.current_us = duration_us;
    if (statistics.cycles == 0U) {
        statistics.minimum_us = duration_us;
        statistics.maximum_us = duration_us;
        statistics.average_us = duration_us;
        statistics.cycles = 1U;
        return;
    }

    if (duration_us < statistics.minimum_us) {
        statistics.minimum_us = duration_us;
    }
    if (duration_us > statistics.maximum_us) {
        statistics.maximum_us = duration_us;
    }

    const double accumulated = statistics.average_us * static_cast<double>(statistics.cycles);
    const std::uint64_t next_cycle = statistics.cycles + 1U;
    statistics.average_us = (accumulated + duration_us) / static_cast<double>(next_cycle);
    statistics.cycles = next_cycle;
}

// Returns (to - from) in microseconds as a double.
double elapsedMicroseconds(const timespec &from, const timespec &to) noexcept {
    return static_cast<double>(toNanoseconds(to) - toNanoseconds(from)) / 1000.0;
}

}  // namespace

// Copies one cycle of process data between the EtherCAT domains and shared
// memory: input flows domain -> pd_input, output flows pd_output -> domain.
void copyProcessData(const std::uint8_t *domain_input,
                     std::size_t input_size,
                     void *shared_input,
                     const void *shared_output,
                     std::uint8_t *domain_output,
                     std::size_t output_size) noexcept {
    if (input_size > 0U && domain_input != nullptr && shared_input != nullptr) {
        std::memcpy(shared_input, domain_input, input_size);
    }
    if (output_size > 0U && shared_output != nullptr && domain_output != nullptr) {
        std::memcpy(domain_output, shared_output, output_size);
    }
}

// Computes the next absolute deadline one period after the previous one. If the
// current time has already passed one or more periods, it skips ahead to the
// first future deadline and reports the missed interval count instead of
// catching up on every skipped cycle.
timespec advanceDeadline(const timespec &previous_deadline,
                         std::uint32_t period_us,
                         const timespec &now,
                         std::uint64_t &missed_intervals) noexcept {
    missed_intervals = 0U;
    if (period_us == 0U) {
        return previous_deadline;
    }

    const std::int64_t period_ns = static_cast<std::int64_t>(period_us) * 1000LL;
    const std::int64_t previous_ns = toNanoseconds(previous_deadline);
    const std::int64_t now_ns = toNanoseconds(now);

    std::int64_t intervals = 1;
    if (now_ns >= previous_ns + period_ns) {
        const std::int64_t elapsed = now_ns - previous_ns;
        intervals = (elapsed / period_ns) + 1;
        missed_intervals = static_cast<std::uint64_t>(intervals - 1);
    }

    return fromNanoseconds(previous_ns + (intervals * period_ns));
}

#if ROCOS_IGH_BUILD_MASTER
// Publishes one cycle's state and timing into the shared EcatBus snapshot.
// Honors a pending resetCycleTime request and marks is_authorized only when the
// link is up, all slaves respond, and both domain working counters are complete.
void updateSharedBus(const BusState &state,
                     CycleStatistics &statistics,
                     std::uint32_t period_us,
                     long monotonic_timestamp_us,
                     EcatBus &bus) noexcept {
    const double current_cycle_us = statistics.current_us;
    if (bus.resetCycleTime) {
        resetTimingStatistics(statistics);
        bus.resetCycleTime = false;
    }

    accountCycleDuration(statistics, current_cycle_us);

    bus.timestamp = monotonic_timestamp_us;
    bus.dt = period_us;
    bus.min_cycle_time = statistics.minimum_us;
    bus.max_cycle_time = statistics.maximum_us;
    bus.avg_cycle_time = statistics.average_us;
    bus.current_cycle_time = statistics.current_us;
    bus.current_state = static_cast<int>(state.al_states);

    bus.is_authorized = state.link_up &&
                        state.responding_slaves == static_cast<unsigned int>(bus.slave_num) &&
                        state.input_wc_state == EC_WC_COMPLETE &&
                        state.output_wc_state == EC_WC_COMPLETE;
}

// Binds the task to an initialized master and mapped IPC for the given period.
CyclicTask::CyclicTask(EthercatMaster &master, SharedMemoryConfig &ipc, std::uint32_t period_us) noexcept
    : master_(master), ipc_(ipc), period_us_(period_us) {}

// Runs the absolute-time cyclic loop until stop_requested is set. Each wake
// performs receive/process, the two fixed-buffer copies, state publication,
// queue/send, and client notification, then advances the deadline.
int CyclicTask::run(volatile std::sig_atomic_t &stop_requested) noexcept {
    if (period_us_ < kMinPeriodUs) {
        return EINVAL;
    }

    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return errno;
    }

    timespec deadline = fromNanoseconds(toNanoseconds(now) + (static_cast<std::int64_t>(period_us_) * 1000LL));
    timespec last_wake = now;

    while (!stop_requested) {
        int sleep_rc = 0;
        do {
            sleep_rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
        } while (sleep_rc == EINTR && !stop_requested);

        if (stop_requested) {
            break;
        }
        if (sleep_rc != 0) {
            return sleep_rc;
        }

        timespec wake_time{};
        if (clock_gettime(CLOCK_MONOTONIC, &wake_time) != 0) {
            return errno;
        }

        statistics_.current_us = elapsedMicroseconds(last_wake, wake_time);

        master_.receiveAndProcess();
        copyProcessData(master_.inputData(),
                        master_.inputSize(),
                        ipc_.pdInputPtr,
                        ipc_.pdOutputPtr,
                        master_.outputData(),
                        master_.outputSize());

        const BusState state = master_.readState();
        if (ipc_.ecatBus != nullptr) {
            updateSharedBus(state, statistics_, period_us_, toMicroseconds(wake_time), *ipc_.ecatBus);
        }

        master_.queueAndSend();
        (void)ipc_.notifyClients();

        std::uint64_t missed = 0U;
        deadline = advanceDeadline(deadline, period_us_, wake_time, missed);
        statistics_.missed_deadlines += missed;
        last_wake = wake_time;
    }

    return 0;
}

// Returns the accumulated cycle statistics collected during run().
const CycleStatistics &CyclicTask::statistics() const noexcept {
    return statistics_;
}

#else
// Hardware-free build: there is no EtherCAT master to drive, so the cyclic
// task degrades to stubs that keep the IPC-only library linkable.

// No shared bus state to publish in this configuration.
void updateSharedBus(const BusState &,
                     CycleStatistics &,
                     std::uint32_t,
                     long,
                     EcatBus &) noexcept {}

// Binds the task to an initialized master and mapped IPC for the given period.
CyclicTask::CyclicTask(EthercatMaster &master, SharedMemoryConfig &ipc, std::uint32_t period_us) noexcept
    : master_(master), ipc_(ipc), period_us_(period_us) {}

// Rejects sub-1ms periods, then reports the feature as unsupported.
int CyclicTask::run(volatile std::sig_atomic_t &) noexcept {
    if (period_us_ < kMinPeriodUs) {
        return EINVAL;
    }
    return ENOTSUP;
}

// Returns the (always empty) cycle statistics collected during run().
const CycleStatistics &CyclicTask::statistics() const noexcept {
    return statistics_;
}
#endif

}  // namespace rocos
