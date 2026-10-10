#pragma once

#include <string>

namespace rocos {

// Initialization only. Probes binding on a temporary thread without changing the
// caller's affinity. Rejects CPUs outside cpu_set_t or denied by the kernel
// (offline, cpuset restrictions, permissions). May expand inherited affinity.
bool validateThreadCpu(unsigned int cpu, std::string &error);

// Initialization only. Binds the calling thread, then verifies the effective
// mask. Does not change other threads, IRQ affinity, or IgH kernel threads.
// Explicit binding may expand inherited affinity; cpuset restrictions still apply.
bool bindCurrentThreadCpu(unsigned int cpu, std::string &error);

} // namespace rocos
