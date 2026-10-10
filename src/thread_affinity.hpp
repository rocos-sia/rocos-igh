#pragma once

#include <string>

namespace rocos {

// Initialization only. Rejects CPUs outside cpu_set_t or the calling thread's
// current allowed mask (including taskset/cpuset restrictions). No side effects.
bool validateThreadCpu(unsigned int cpu, std::string &error);

// Initialization only. Binds the calling thread, then verifies the effective
// mask. Does not change other threads, IRQ affinity, or IgH kernel threads.
bool bindCurrentThreadCpu(unsigned int cpu, std::string &error);

} // namespace rocos
