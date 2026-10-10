#include "thread_affinity.hpp"

#include <iostream>
#include <limits>
#include <sched.h>
#include <string>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "line " << __LINE__ << ": " << #condition << '\n'; return false; \
} } while (false)

bool testAffinity() {
    cpu_set_t original;
    CPU_ZERO(&original);
    CHECK(sched_getaffinity(0, sizeof(original), &original) == 0);
    unsigned int selected = CPU_SETSIZE;
    for (unsigned int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &original)) { selected = cpu; break; }
    }
    CHECK(selected < CPU_SETSIZE);
    std::string error;
    CHECK(rocos::validateThreadCpu(selected, error));
    CHECK(!rocos::bindCurrentThreadCpu(CPU_SETSIZE, error));
    CHECK(!rocos::validateThreadCpu(std::numeric_limits<unsigned int>::max(), error));

    // Exercise real Linux affinity without RT privileges or EtherCAT hardware.
    // Thread exit restores the test environment; the parent mask must not change.
    bool worker_ok = false;
    std::thread worker([&] {
        worker_ok = [&]() -> bool {
            // Discover kernel-permitted CPUs independently of inherited affinity.
            // Only this disposable test thread changes its mask.
            cpu_set_t available;
            CPU_ZERO(&available);
            for (unsigned int cpu = 0; cpu < CPU_SETSIZE; ++cpu) CPU_SET(cpu, &available);
            CHECK(sched_setaffinity(0, sizeof(available), &available) == 0);
            CHECK(sched_getaffinity(0, sizeof(available), &available) == 0);
            CHECK(rocos::bindCurrentThreadCpu(selected, error));
            cpu_set_t actual;
            CPU_ZERO(&actual);
            CHECK(sched_getaffinity(0, sizeof(actual), &actual) == 0);
            CHECK(CPU_COUNT(&actual) == 1 && CPU_ISSET(selected, &actual));
            CHECK(sched_getcpu() == static_cast<int>(selected));
            // The probe inherits a singleton mask but must be able to bind another
            // permitted CPU, leaving its caller unchanged (as with the reporter).
            unsigned int alternative = CPU_SETSIZE;
            unsigned int unavailable = CPU_SETSIZE;
            for (unsigned int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (CPU_ISSET(cpu, &available) && cpu != selected) alternative = cpu;
                if (!CPU_ISSET(cpu, &available)) unavailable = cpu;
            }
            if (unavailable < CPU_SETSIZE) {
                CHECK(!rocos::validateThreadCpu(unavailable, error));
                CHECK(error.find("sched_setaffinity CPU") != std::string::npos);
                CHECK(error.find("cpuset") != std::string::npos);
                CHECK(!rocos::bindCurrentThreadCpu(unavailable, error));
                CHECK(sched_getaffinity(0, sizeof(actual), &actual) == 0);
                CHECK(CPU_COUNT(&actual) == 1 && CPU_ISSET(selected, &actual));
            }
            if (alternative < CPU_SETSIZE) {
                CHECK(rocos::validateThreadCpu(alternative, error));
                CHECK(error.empty());
                CHECK(sched_getaffinity(0, sizeof(actual), &actual) == 0);
                CHECK(CPU_COUNT(&actual) == 1 && CPU_ISSET(selected, &actual));
                CHECK(rocos::bindCurrentThreadCpu(alternative, error));
                CHECK(sched_getaffinity(0, sizeof(actual), &actual) == 0);
                CHECK(CPU_COUNT(&actual) == 1 && CPU_ISSET(alternative, &actual));
                CHECK(sched_getcpu() == static_cast<int>(alternative));
            } else {
                std::cout << "SKIP affinity expansion: only one CPU permitted by kernel\n";
            }
            CHECK(rocos::bindCurrentThreadCpu(selected, error));
            CHECK(error.empty());
            return true;
        }();
    });
    worker.join();
    CHECK(worker_ok);
    cpu_set_t after;
    CPU_ZERO(&after);
    CHECK(sched_getaffinity(0, sizeof(after), &after) == 0);
    CHECK(CPU_EQUAL(&original, &after));
    return true;
}

int main() { return testAffinity() ? 0 : 1; }
