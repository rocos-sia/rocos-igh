#include "cyclic_task.hpp"
#include "ethercat_master.hpp"
#include "runtime_options.hpp"
#include "shared_memory_config.hpp"
#include "slave_config.hpp"

#include <array>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sched.h>
#include <string>
#include <sys/mman.h>

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

constexpr std::size_t kPrefaultStackBytes = 64U * 1024U;
constexpr int kRealtimePriority = 80;

void handleSignal(int) noexcept {
    g_stop_requested = 1;
}

bool installSignalHandlers(std::string &error) {
    error.clear();

    struct sigaction action {};
    action.sa_handler = handleSignal;
    if (sigemptyset(&action.sa_mask) != 0) {
        error = "sigemptyset failed";
        return false;
    }

    if (sigaction(SIGINT, &action, nullptr) != 0) {
        error = "sigaction SIGINT failed";
        return false;
    }
    if (sigaction(SIGTERM, &action, nullptr) != 0) {
        error = "sigaction SIGTERM failed";
        return false;
    }

    return true;
}

bool configureRealtime(std::string &error) {
    error.clear();

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        error = "mlockall failed: " + std::string(std::strerror(errno));
        return false;
    }

    std::array<unsigned char, kPrefaultStackBytes> stack{};
    volatile unsigned char *const stack_data = stack.data();
    for (std::size_t i = 0; i < stack.size(); i += 4096U) {
        stack_data[i] = 0;
    }

    sched_param scheduler{};
    scheduler.sched_priority = kRealtimePriority;
    if (sched_setscheduler(0, SCHED_FIFO, &scheduler) != 0) {
        error = "sched_setscheduler failed: " + std::string(std::strerror(errno));
        return false;
    }

    return true;
}

void printFinalStatistics(const rocos::CycleStatistics &stats, int run_result) {
    std::cout << "final_stats cycles=" << stats.cycles
              << " missed_deadlines=" << stats.missed_deadlines
              << " min_us=" << stats.minimum_us
              << " max_us=" << stats.maximum_us
              << " avg_us=" << stats.average_us
              << " current_us=" << stats.current_us
              << " run_rc=" << run_result << '\n';
}

}  // namespace

int main(int argc, char **argv) {
    rocos::RuntimeOptions options{};
    std::string error;
    if (!rocos::parseRuntimeOptions(argc, argv, options, error)) {
        std::cerr << error << '\n' << rocos::runtimeOptionsUsage() << '\n';
        return EXIT_FAILURE;
    }
    if (options.show_help) {
        std::cout << rocos::runtimeOptionsUsage() << '\n';
        return EXIT_SUCCESS;
    }

    if (!installSignalHandlers(error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    rocos::StaticSlaveConfig config = rocos::defaultSlaveConfig();
    if (config.slave_count == 0U) {
        std::cerr << "no slave configuration compiled" << '\n';
        return EXIT_FAILURE;
    }

    rocos::EthercatMaster master;
    if (!master.initialize(options.master_id, config, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    rocos::SharedMemoryConfig ipc(static_cast<int>(options.master_id));
    if (!ipc.createSharedMemory()) {
        std::cerr << "failed to create shared memory" << '\n';
        return EXIT_FAILURE;
    }
    if (!ipc.createPdDataMemoryProvider(static_cast<int>(master.inputSize()), static_cast<int>(master.outputSize()))) {
        std::cerr << "failed to create process data memory" << '\n';
        return EXIT_FAILURE;
    }

    if (!rocos::publishConfig(*ipc.ecatBus, config, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    ipc.ecatBus->dt = options.period_us;
    ipc.ecatBus->current_state = ECAT_STATE_INIT;
    ipc.ecatBus->is_authorized = false;
    ipc.ecatBus->timestamp = 0;

    if (!configureRealtime(error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    rocos::CyclicTask cyclic_task(master, ipc, options.period_us);
    const int run_result = cyclic_task.run(g_stop_requested);
    printFinalStatistics(cyclic_task.statistics(), run_result);

    if (run_result != 0) {
        std::cerr << "cyclic task failed: " << std::strerror(run_result) << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
