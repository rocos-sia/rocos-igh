#include "thread_affinity.hpp"

#include <cerrno>
#include <cstring>
#include <sched.h>

namespace rocos {

bool validateThreadCpu(unsigned int cpu, std::string &error) {
    error.clear();
    if (cpu >= CPU_SETSIZE) {
        error = "--cpu must be below CPU_SETSIZE (" + std::to_string(CPU_SETSIZE) + ")";
        return false;
    }
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        error = "sched_getaffinity failed: " + std::string(std::strerror(errno));
        return false;
    }
    if (!CPU_ISSET(cpu, &allowed)) {
        error = "--cpu " + std::to_string(cpu) +
                " is not in the current allowed CPU mask; check online CPUs, taskset and cpuset";
        return false;
    }
    return true;
}

bool bindCurrentThreadCpu(unsigned int cpu, std::string &error) {
    if (!validateThreadCpu(cpu, error)) return false;
    cpu_set_t requested;
    CPU_ZERO(&requested);
    CPU_SET(cpu, &requested);
    if (sched_setaffinity(0, sizeof(requested), &requested) != 0) {
        error = "sched_setaffinity CPU " + std::to_string(cpu) +
                " failed: " + std::string(std::strerror(errno));
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
