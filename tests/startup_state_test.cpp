#include "ethercat_master.hpp"
#include "startup_wait.hpp"
#include "cyclic_task.hpp"
#include "shared_memory_config.hpp"

#include <cerrno>
#include <iostream>

namespace {
ec_slave_config_state_t states[2]{};
int results[2]{};
unsigned int queries[2]{};
}

extern "C" int __wrap_ecrt_slave_config_state(
    const ec_slave_config_t *sc, ec_slave_config_state_t *state) {
    const auto index = reinterpret_cast<std::uintptr_t>(sc) - 1U;
    ++queries[index];
    *state = states[index];
    return results[index];
}

namespace rocos {
struct EthercatMasterTestPeer {
    static bool waitPreop(const StaticSlaveConfig &config, std::string &error,
        const std::function<int(std::uint16_t, ec_slave_info_t &)> &query,
        const std::function<void()> &wait, std::size_t attempts,
        const std::function<bool()> &expired = {}) {
        return EthercatMaster::waitForSlavesInPreop(config, error, query, wait, attempts, expired);
    }
    static void seed(EthercatMaster &master) {
        master.initialized_ = true;
        master.slave_configs_ = {reinterpret_cast<ec_slave_config_t *>(1),
                                 reinterpret_cast<ec_slave_config_t *>(2)};
    }
};
}

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return 1; } } while (false)

int main() {
    using Result = rocos::StartupWait::Result;
    rocos::StartupWait gate(0);
    // Activation success and ten exchanged frames are not readiness.
    for (int i = 0; i < 100; ++i) {
        CHECK(gate.observe(i * 1000000LL, false) == Result::Waiting);
    }
    for (int i = 0; i < 4; ++i) {
        CHECK(gate.observe(200000000LL + i, true) == Result::Waiting);
    }
    CHECK(gate.observe(300000000LL, false) == Result::Waiting);
    for (int i = 0; i < 4; ++i) {
        CHECK(gate.observe(400000000LL + i, true) == Result::Waiting);
    }
    CHECK(gate.observe(500000000LL, true) == Result::Ready);
    rocos::StartupWait timeout(100);
    CHECK(timeout.observe(10000000099LL, false) == Result::Waiting);
    CHECK(timeout.observe(10000000100LL, true) == Result::TimedOut);
    rocos::StartupWait late_wake(0);
    CHECK(late_wake.observe(20000000000LL, false) == Result::TimedOut);

    rocos::StartupWait custom_timeout(100, 30000U);
    CHECK(custom_timeout.observe(10000000100LL, false) == Result::Waiting);
    CHECK(custom_timeout.observe(30000000099LL, false) == Result::Waiting);
    CHECK(custom_timeout.observe(30000000100LL, true) == Result::TimedOut);
    rocos::StartupWait short_timeout(0, 1U);
    CHECK(short_timeout.observe(999999LL, false) == Result::Waiting);
    CHECK(short_timeout.observe(1000000LL, true) == Result::TimedOut);
    rocos::StartupWait max_timeout(0, UINT32_MAX);
    CHECK(max_timeout.observe(4294967295000000LL - 1, false) == Result::Waiting);
    CHECK(max_timeout.observe(4294967295000000LL, false) == Result::TimedOut);

    rocos::EthercatMaster master;
    bool ready = true;
    CHECK(master.pollSlaves(EC_AL_STATE_OP, ready) == 0 && !ready);
    rocos::EthercatMasterTestPeer::seed(master);
    states[0] = {1, 1, EC_AL_STATE_OP};
    states[1] = {1, 0, EC_AL_STATE_SAFEOP};
    CHECK(master.pollSlaves(EC_AL_STATE_OP, ready) == 0 && !ready);
    CHECK(queries[0] == 1 && queries[1] == 1);
    states[1] = {1, 1, EC_AL_STATE_OP};
    CHECK(master.pollSlaves(EC_AL_STATE_OP, ready) == 0 && ready);
    states[0].online = 0;
    CHECK(master.pollSlaves(EC_AL_STATE_OP, ready) == 0 && !ready);
    states[0].online = 1;
    states[0].operational = 0;
    CHECK(master.pollSlaves(EC_AL_STATE_OP, ready) == 0 && !ready);
    states[0].operational = 1;
    results[0] = -EIO;
    CHECK(master.pollSlaves(EC_AL_STATE_OP, ready) == -EIO && !ready);
    // A failure at slave 0 must not skip polling slave 1.
    CHECK(queries[0] == 5 && queries[1] == 5);

    // Snapshot includes only online, operational OP slaves, and respects capacity.
    bool snapshot[3]{false, false, true};
    results[0] = 0;
    states[1] = {1, 0, EC_AL_STATE_SAFEOP};
    CHECK(master.pollSlaves(EC_AL_STATE_OP, ready, snapshot, 2) == 0 && !ready);
    CHECK(snapshot[0] && !snapshot[1] && snapshot[2]);
    states[0].online = 0;
    states[1] = {1, 1, EC_AL_STATE_OP};
    CHECK(master.pollSlaves(EC_AL_STATE_OP, ready, snapshot, 2) == 0 && !ready);
    CHECK(!snapshot[0] && snapshot[1]);
    states[0].online = 1;
    results[1] = -EIO;
    CHECK(master.pollSlaves(EC_AL_STATE_OP, ready, snapshot, 2) == -EIO && !ready);
    CHECK(snapshot[0] && !snapshot[1]);
    snapshot[1] = true;
    CHECK(master.pollSlaves(EC_AL_STATE_OP, ready, snapshot, 1) == -EIO);
    CHECK(snapshot[1]);

    // PREOP must be simultaneous, error-free, and stable before configuration.
    rocos::SlaveSpec slaves[2]{};
    slaves[1].position = 1;
    rocos::StaticSlaveConfig config{slaves, 2};
    std::size_t round = 0;
    std::string error;
    const auto preop_query = [&](std::uint16_t position, ec_slave_info_t &info) {
        info.al_state = EC_AL_STATE_PREOP;
        info.error_flag = (position == 1 && round == 3);
        return 0;
    };
    CHECK(rocos::EthercatMasterTestPeer::waitPreop(
        config, error, preop_query, [&] { ++round; }, 9));
    CHECK(round == 8);
    round = 0;
    CHECK(!rocos::EthercatMasterTestPeer::waitPreop(
        config, error, preop_query, [&] { ++round; }, 8));

    // A slow query consumes the timeout, including the fifth healthy poll.
    std::int64_t elapsed_ms = 0;
    const auto slow_query = [&](std::uint16_t, ec_slave_info_t &info) {
        info.al_state = EC_AL_STATE_PREOP;
        info.error_flag = 0;
        elapsed_ms += 3;
        return 0;
    };
    const auto fake_wait = [&] { elapsed_ms += 10; };
    CHECK(!rocos::EthercatMasterTestPeer::waitPreop(
        config, error, slow_query, fake_wait, 100, [&] { return elapsed_ms >= 70; }));
    CHECK(elapsed_ms == 70);
    CHECK(error.find("timed out") != std::string::npos);
    elapsed_ms = 0;
    CHECK(rocos::EthercatMasterTestPeer::waitPreop(
        config, error, slow_query, fake_wait, 100, [&] { return elapsed_ms >= 71; }));

    // Complete WCs alone must not mark a SAFEOP/mixed-state bus healthy.
    rocos::CycleStatistics statistics{};
    rocos::EcatBus bus{};
    bus.slave_num = 2;
    rocos::BusState state{};
    state.link_up = true;
    state.responding_slaves = 2;
    state.input_wc_state = EC_WC_COMPLETE;
    state.output_wc_state = EC_WC_COMPLETE;
    state.al_states = EC_AL_STATE_OP;
    rocos::updateSharedBus(state, statistics, 1000, 0, bus);
    CHECK(bus.is_authorized);
    state.al_states = EC_AL_STATE_OP | EC_AL_STATE_SAFEOP;
    rocos::updateSharedBus(state, statistics, 1000, 0, bus);
    CHECK(!bus.is_authorized);
    state.al_states = EC_AL_STATE_OP;
    state.output_wc_state = EC_WC_INCOMPLETE;
    rocos::updateSharedBus(state, statistics, 1000, 0, bus);
    CHECK(!bus.is_authorized);
    state.output_wc_state = EC_WC_COMPLETE;
    state.link_up = false;
    rocos::updateSharedBus(state, statistics, 1000, 0, bus);
    CHECK(!bus.is_authorized);
    return 0;
}
