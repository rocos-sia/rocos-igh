#include "thread_affinity.hpp"

#include <cerrno>
#include <cstring>
#include <sched.h>
#include <system_error>
#include <thread>

namespace rocos {

bool validateThreadCpu(unsigned int cpu, std::string &error) {
    // Probe on a disposable thread before accessing EtherCAT. Inherited affinity
    // is not a cpuset boundary: an explicitly requested isolated CPU may lie
    // outside it. Keep the caller's mask intact for future reporter threads.
    if (cpu >= CPU_SETSIZE) {
        error = "--cpu must be below CPU_SETSIZE (" + std::to_string(CPU_SETSIZE) + ")";
        return false;
    }
    bool valid = false;
    try {
        std::thread probe([&] { valid = bindCurrentThreadCpu(cpu, error); });
        probe.join();
    } catch (const std::system_error &exception) {
        error = "CPU affinity probe failed: " + std::string(exception.what());
        return false;
    }
    return valid;
}

bool bindCurrentThreadCpu(unsigned int cpu, std::string &error) {
    error.clear();
    if (cpu >= CPU_SETSIZE) {
        error = "--cpu must be below CPU_SETSIZE (" + std::to_string(CPU_SETSIZE) + ")";
        return false;
    }
    cpu_set_t requested;
    CPU_ZERO(&requested);
    CPU_SET(cpu, &requested);
    if (sched_setaffinity(0, sizeof(requested), &requested) != 0) {
        error = "sched_setaffinity CPU " + std::to_string(cpu) +
                " failed: " + std::string(std::strerror(errno)) +
                "; check online CPUs, cpuset restrictions and affinity permissions";
        return false;
    }
    cpu_set_t effective;
    CPU_ZERO(&effective);
    if (sched_getaffinity(0, sizeof(effective), &effective) != 0) {
        error = "sched_getaffinity after binding failed: " + std::string(std::strerror(errno));
        return false;
    }
    if (!CPU_EQUAL(&requested, &effective)) {
        error = "effective CPU affinity differs from requested CPU " + std::to_string(cpu);
        return false;
    }
    return true;
}

} // namespace rocos
