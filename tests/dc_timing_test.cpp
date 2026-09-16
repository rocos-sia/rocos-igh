#include "ethercat_master.hpp"
#include <cerrno>
#include <ctime>
#include <iostream>
#include <vector>

namespace {
std::vector<int> calls;
std::uint64_t application_time = 0, reference_time = 0;
int clock_error = 0, reference_error = 0;
}
extern "C" int __wrap_clock_gettime(clockid_t, timespec *time) {
    calls.push_back(2);
    if (clock_error) { errno = clock_error; return -1; }
    *time = {12, 345};
    return 0;
}
extern "C" int __wrap_ecrt_master_application_time(ec_master_t *, std::uint64_t time) {
    calls.push_back(1); application_time = time; return 0;
}
extern "C" int __wrap_ecrt_master_sync_reference_clock_to(ec_master_t *, std::uint64_t time) {
    calls.push_back(3); reference_time = time; return reference_error;
}
extern "C" int __wrap_ecrt_master_sync_slave_clocks(ec_master_t *) {
    calls.push_back(4); return 0;
}
extern "C" int __wrap_ecrt_domain_queue(ec_domain_t *) { return 0; }
extern "C" int __wrap_ecrt_master_send(ec_master_t *) { calls.push_back(5); return 0; }
extern "C" void __wrap_ecrt_release_master(ec_master_t *) {}

namespace rocos {
struct EthercatMasterTestPeer {
    static void seed(EthercatMaster &m, bool dc) {
        m.initialized_ = true;
        m.dc_enabled_ = dc;
        m.master_ = reinterpret_cast<ec_master_t *>(1);
        m.input_domain_ = reinterpret_cast<ec_domain_t *>(2);
        m.output_domain_ = reinterpret_cast<ec_domain_t *>(3);
    }
};
}
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return 1; } } while (false)
int main() {
    rocos::EthercatMaster master;
    rocos::EthercatMasterTestPeer::seed(master, true);
    CHECK(master.setApplicationTime(12000000000ULL));
    CHECK(master.queueAndSend());
    CHECK(application_time == 12000000000ULL);
    CHECK(reference_time == 12000000345ULL);
    CHECK(calls == std::vector<int>({1, 2, 3, 4, 5}));
    calls.clear();
    clock_error = EIO;
    CHECK(!master.queueAndSend());
    CHECK(calls == std::vector<int>({2}));
    CHECK(master.lastDcError().error_code == -EIO);
    calls.clear();
    clock_error = 0;
    reference_error = -ENXIO;
    CHECK(!master.queueAndSend());
    CHECK(calls == std::vector<int>({2, 3}));
    CHECK(master.lastDcError().stage == rocos::DcErrorStage::SyncReferenceClock);
    calls.clear();
    rocos::EthercatMasterTestPeer::seed(master, false);
    CHECK(master.setApplicationTime(42));
    CHECK(master.queueAndSend());
    CHECK(calls == std::vector<int>({5}));
    return 0;
}
