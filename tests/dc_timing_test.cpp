#include "ethercat_master.hpp"
#include "cyclic_task.hpp"
#include <cerrno>
#include <ctime>
#include <iostream>
#include <vector>

namespace {
std::vector<int> calls;
std::uint64_t application_time = 0, reference_time = 0;
int clock_error = 0, reference_error = 0;
int activate_error = 0, app_error = 0, app_fail_at = 0, app_calls = 0;
int clock_calls = 0, clock_fail_at = 0;
std::uint64_t clock_ns = 12000000345ULL;
}
extern "C" int __wrap_clock_gettime(clockid_t, timespec *time) {
    calls.push_back(2);
    ++clock_calls;
    if (clock_fail_at == clock_calls) { errno = EIO; return -1; }
    if (clock_error) { errno = clock_error; return -1; }
    *time = {static_cast<time_t>(clock_ns / 1000000000ULL),
             static_cast<long>(clock_ns % 1000000000ULL)};
    return 0;
}
extern "C" int __wrap_ecrt_master_application_time(ec_master_t *, std::uint64_t time) {
    calls.push_back(1); application_time = time;
    return ++app_calls == app_fail_at ? app_error : 0;
}
extern "C" int __wrap_ecrt_master_activate(ec_master_t *) {
    calls.push_back(6); clock_ns += 2500000ULL; return activate_error;
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
    static bool activate(EthercatMaster &m, bool dc, std::string &error) {
        return m.activateWithDcTime(dc, error);
    }
    static void reset(EthercatMaster &m) { m.reset(); }
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
    std::string error;
    calls.clear();
    CHECK(rocos::EthercatMasterTestPeer::activate(master, false, error));
    CHECK(calls == std::vector<int>({6}));
    CHECK(master.dcPhaseOriginNs() == 0);
    calls.clear();
    clock_ns = 12000000345ULL;
    CHECK(rocos::EthercatMasterTestPeer::activate(master, true, error));
    CHECK(calls == std::vector<int>({2, 1, 6, 2, 1}));
    CHECK(master.dcPhaseOriginNs() == 12000000345ULL);
    CHECK(application_time == 12002500345ULL);
    // IPC setup spans multiple periods: first deadline remains on seed phase.
    std::uint64_t skipped = 0;
    const timespec origin{12, 345};
    const timespec after_setup{12, 7654321};
    const auto deadline = rocos::advanceDeadline(origin, 1000, after_setup, skipped);
    CHECK(deadline.tv_sec == 12 && deadline.tv_nsec == 8000345);
    CHECK((static_cast<std::uint64_t>(deadline.tv_sec) * 1000000000ULL +
           deadline.tv_nsec - master.dcPhaseOriginNs()) % 1000000ULL == 0);
    for (int stage = 1; stage <= 5; ++stage) {
        calls.clear(); app_calls = 0; clock_calls = 0;
        app_fail_at = stage == 2 ? 1 : (stage == 5 ? 2 : 0);
        app_error = -EIO;
        clock_fail_at = stage == 1 ? 1 : (stage == 4 ? 2 : 0);
        activate_error = stage == 3 ? -EIO : 0;
        CHECK(!rocos::EthercatMasterTestPeer::activate(master, true, error));
        CHECK(!error.empty());
        const std::vector<int> sequence{2, 1, 6, 2, 1};
        CHECK(calls == std::vector<int>(sequence.begin(), sequence.begin() + stage));
        if (app_fail_at) {
            CHECK(master.lastDcError().stage == rocos::DcErrorStage::ApplicationTime);
            CHECK(master.lastDcError().error_code == -EIO);
        }
    }
    rocos::EthercatMasterTestPeer::reset(master);
    CHECK(master.dcPhaseOriginNs() == 0);
    return 0;
}
