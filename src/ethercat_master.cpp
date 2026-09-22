#include "ethercat_master.hpp"

#include "shared_memory_config.hpp"

#include <algorithm>
#include <limits>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <thread>
#include <vector>

namespace rocos {

namespace {

constexpr auto kPreopPollInterval = std::chrono::milliseconds(10);
// Number of consecutive all-PREOP polls required before proceeding with
// PDO configuration. This guards against the IgH kernel module transiently
// setting error_flag while driving a slave from OP back to PREOP: the slave
// appears ready on one sample but is not yet stable enough for PDO remapping.
constexpr std::size_t kPreopStableConfirmations = 5U;

const char *alStateName(std::uint8_t al_state) noexcept {
    switch (al_state) {
    case EC_AL_STATE_INIT:   return "INIT";
    case EC_AL_STATE_PREOP:  return "PREOP";
    case EC_AL_STATE_SAFEOP: return "SAFEOP";
    case EC_AL_STATE_OP:     return "OP";
    default:                 return "UNKNOWN";
    }
}

}  // namespace

const char *dcErrorStageName(DcErrorStage stage) noexcept {
    switch (stage) {
    case DcErrorStage::ApplicationTime:
        return "ecrt_master_application_time";
    case DcErrorStage::SyncReferenceClock:
        return "ecrt_master_sync_reference_clock_to";
    case DcErrorStage::SyncSlaveClocks:
        return "ecrt_master_sync_slave_clocks";
    case DcErrorStage::None:
        return "none";
    }
    return "unknown";
}

// Releases the requested IgH master and drops all borrowed pointers.
EthercatMaster::~EthercatMaster() {
    reset();
}

// Requests the IgH master, creates one input and one output domain, configures
// each slave and registers its PDO entries, then activates the master and caches
// both domain buffers and sizes. Fails fast with reset() on any error.
bool EthercatMaster::initialize(unsigned int master_id,
                                StaticSlaveConfig config,
                                bool dc_enabled,
                                std::uint32_t period_us,
                                std::string &error,
                                std::uint32_t preop_timeout_ms) {
    error.clear();
    last_dc_error_ = DcError{};

    if (preop_timeout_ms == 0U) {
        error = "PREOP timeout must be positive";
        return false;
    }
    if (initialized_) {
        error = "master already initialized";
        return false;
    }
    if (config.slave_count == 0) {
        error = "no slave configuration compiled";
        reset();
        return false;
    }
    if (!validateSlaveConfig(config, error)) {
        reset();
        return false;
    }

    DistributedClockRuntimeConfig dc_runtime{};
    if (!buildDistributedClockRuntimeConfig(config, dc_enabled, period_us,
                                             dc_runtime, error)) {
        reset();
        return false;
    }

    master_ = ecrt_request_master(master_id);
    if (master_ == nullptr) {
        error = "failed to request EtherCAT master";
        reset();
        return false;
    }

    // IgH requests PREOP asynchronously when the master is reserved. Activating
    // while a slave is still OP can overwrite that request and skip reconfiguration.
    // Print current state of every slave before the wait loop so any non-PREOP
    // slaves are visible in the log from the start.
    for (std::size_t slave_index = 0; slave_index < config.slave_count; ++slave_index) {
        ec_slave_info_t slave_info{};
        if (ecrt_master_get_slave(master_, config.slaves[slave_index].position,
                                  &slave_info) == 0) {
            const std::uint8_t al = slave_info.al_state;
            if (al != EC_AL_STATE_PREOP) {
                std::cout << "slave[" << slave_index << "] is in "
                          << alStateName(al) << " at startup"
                          << " — waiting for IgH to drive it to PREOP\n";
            }
        }
    }
    if (!waitForSlavesInPreop(config, error, preop_timeout_ms)) {
        reset();
        return false;
    }

    slave_configs_.reserve(config.slave_count);
    ec_slave_config_t *reference_clock_config = nullptr;
    for (std::size_t slave_index = 0; slave_index < config.slave_count; ++slave_index) {
        SlaveSpec &slave = config.slaves[slave_index];

        ec_slave_info_t slave_info{};
        if (ecrt_master_get_slave(master_, slave.position, &slave_info) != 0) {
            error = "failed to read SII identity for slave[" + std::to_string(slave_index) + "]";
            reset();
            return false;
        }

        if (slave.vendor_id == 0) {
            // Auto-discover identity from SII.
            if (!applyDiscoveredIdentity(slave, slave_info.vendor_id, slave_info.product_code, error)) {
                error += " for slave[" + std::to_string(slave_index) + "]";
                reset();
                return false;
            }
            if (slave.vendor_id == 0 || slave.product_code == 0) {
                error = "SII identity for slave[" + std::to_string(slave_index) +
                        "] is still zero after discovery";
                reset();
                return false;
            }
        } else {
            // Cross-check configured identity against what the bus actually reports.
            if (slave_info.vendor_id != slave.vendor_id ||
                slave_info.product_code != slave.product_code) {
                error = "SII identity mismatch for slave[" + std::to_string(slave_index) +
                        "]: configured vendor_id=0x" +
                        [](std::uint32_t v) {
                            char buf[12];
                            std::snprintf(buf, sizeof(buf), "%08X", v);
                            return std::string(buf);
                        }(slave.vendor_id) +
                        " product_code=0x" +
                        [](std::uint32_t v) {
                            char buf[12];
                            std::snprintf(buf, sizeof(buf), "%08X", v);
                            return std::string(buf);
                        }(slave.product_code) +
                        ", SII reports vendor_id=0x" +
                        [](std::uint32_t v) {
                            char buf[12];
                            std::snprintf(buf, sizeof(buf), "%08X", v);
                            return std::string(buf);
                        }(slave_info.vendor_id) +
                        " product_code=0x" +
                        [](std::uint32_t v) {
                            char buf[12];
                            std::snprintf(buf, sizeof(buf), "%08X", v);
                            return std::string(buf);
                        }(slave_info.product_code);
                reset();
                return false;
            }
        }
    }

    input_domain_ = ecrt_master_create_domain(master_);
    if (input_domain_ == nullptr) {
        error = "failed to create input domain";
        reset();
        return false;
    }

    output_domain_ = ecrt_master_create_domain(master_);
    if (output_domain_ == nullptr) {
        error = "failed to create output domain";
        reset();
        return false;
    }

    for (std::size_t slave_index = 0; slave_index < config.slave_count; ++slave_index) {
        SlaveSpec &slave = config.slaves[slave_index];
        ec_slave_config_t *sc = ecrt_master_slave_config(
            master_, slave.alias, slave.position, slave.vendor_id, slave.product_code);
        if (sc == nullptr) {
            error = "failed to get slave config";
            reset();
            return false;
        }

        slave_configs_.push_back(sc);

        if (ecrt_slave_config_pdos(sc, EC_END, slave.syncs) != 0) {
            error = "failed to configure slave PDOs";
            reset();
            return false;
        }

        if (dc_runtime.enabled && slave.dc.has_value()) {
            const DistributedClockConfig &dc = *slave.dc;
            if (ecrt_slave_config_dc(sc, dc.assign_activate,
                                     dc_runtime.sync0_cycle_ns, dc.sync0_shift_ns,
                                     dc.sync1_cycle_ns, dc.sync1_shift_ns) != 0) {
                error = "failed to configure DC for slave[" +
                        std::to_string(slave_index) + "]";
                reset();
                return false;
            }
            if (slave_index == dc_runtime.reference_slave_index) {
                reference_clock_config = sc;
            }
        }

        std::size_t entry_index = 0;
        for (unsigned int sync_position = 0;
             slave.syncs[sync_position].index != 0xffU; ++sync_position) {
            const ec_sync_info_t &sync = slave.syncs[sync_position];
            // Only SM2 (RxPDO, master→slave) and SM3 (TxPDO, slave→master) carry
            // process data; skip any other sync managers silently.
            if (sync.index != 2U && sync.index != 3U) {
                continue;
            }
            if ((sync.index == 2U) != (sync.dir == EC_DIR_OUTPUT)) {
                error = "slave[" + std::to_string(slave_index) +
                        "] sync manager " + std::to_string(sync.index) +
                        " direction does not match expected RxPDO/TxPDO assignment";
                reset();
                return false;
            }
            ec_domain_t *domain =
                (sync.dir == EC_DIR_INPUT) ? input_domain_ : output_domain_;
            for (unsigned int pdo_position = 0; pdo_position < sync.n_pdos;
                 ++pdo_position) {
                const ec_pdo_info_t &pdo = sync.pdos[pdo_position];
                for (unsigned int pdo_entry_position = 0;
                     pdo_entry_position < pdo.n_entries;
                     ++pdo_entry_position) {
                    if (entry_index >= slave.entry_count) {
                        error = "PDO entry table does not match Sync Manager mapping";
                        reset();
                        return false;
                    }

                    PdoEntrySpec &entry = slave.entries[entry_index++];
                    unsigned int bit_position = 0;
                    const int offset = ecrt_slave_config_reg_pdo_entry_pos(
                        sc, sync.index, pdo_position, pdo_entry_position,
                        domain, &bit_position);
                    if (offset < 0) {
                        error = "failed to register PDO entry";
                        reset();
                        return false;
                    }
                    if (bit_position != 0U) {
                        error = "pdo entry is not byte-aligned";
                        reset();
                        return false;
                    }

                    entry.offset = static_cast<unsigned int>(offset);
                    entry.bit_position = bit_position;
                }
            }
        }
        if (entry_index != slave.entry_count) {
            error = "PDO entry table does not match Sync Manager mapping";
            reset();
            return false;
        }
    }

    if (dc_runtime.enabled && reference_clock_config == nullptr) {
        error = "DC reference clock configuration was not created";
        reset();
        return false;
    }
    if (dc_runtime.enabled &&
        ecrt_master_select_reference_clock(master_, reference_clock_config) != 0) {
        error = "failed to select DC reference clock for slave[" +
                std::to_string(dc_runtime.reference_slave_index) + "]";
        reset();
        return false;
    }

    // Application time is first supplied by the cyclic task's deadline, so
    // IgH's DC phase origin and the application's schedule share one epoch.
    if (ecrt_master_activate(master_) != 0) {
        error = "failed to activate EtherCAT master";
        reset();
        return false;
    }

    input_data_ = ecrt_domain_data(input_domain_);
    output_data_ = ecrt_domain_data(output_domain_);
    input_size_ = ecrt_domain_size(input_domain_);
    output_size_ = ecrt_domain_size(output_domain_);

    std::cout << "[EthercatMaster] master_id=" << master_id
              << " input_domain_size=" << input_size_ << " bytes"
              << " output_domain_size=" << output_size_ << " bytes\n";

    if (input_data_ == nullptr || output_data_ == nullptr) {
        error = "failed to acquire domain data";
        reset();
        return false;
    }
    if (input_size_ == 0U || output_size_ == 0U) {
        error = "domain size must be non-zero";
        reset();
        return false;
    }
    if (input_size_ > EC_SHM_MAX_SIZE || output_size_ > EC_SHM_MAX_SIZE) {
        error = "domain size exceeds EC_SHM_MAX_SIZE";
        reset();
        return false;
    }

    std::memset(output_data_, 0, output_size_);
    dc_enabled_ = dc_runtime.enabled;
    initialized_ = true;
    return true;
}

bool EthercatMaster::slaveReadyForConfiguration(const ec_slave_info_t &slave_info) noexcept {
    return slave_info.al_state == EC_AL_STATE_PREOP && slave_info.error_flag == 0U;
}

bool EthercatMaster::waitForSlavesInPreop(const StaticSlaveConfig &config,
                                          std::string &error, std::uint32_t timeout_ms) {
    const auto query = [this](std::uint16_t position, ec_slave_info_t &slave_info) {
        return ecrt_master_get_slave(master_, position, &slave_info);
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    const auto expired = [&] { return std::chrono::steady_clock::now() >= deadline; };
    const auto wait = [&] {
        std::this_thread::sleep_until(std::min(deadline,
            std::chrono::steady_clock::now() + kPreopPollInterval));
    };
    const bool ready = waitForSlavesInPreop(config, error, query, wait,
                                           std::numeric_limits<std::size_t>::max(), expired);
    if (!ready) {
        error += " (PREOP timeout_ms=" + std::to_string(timeout_ms) + ")";
    }
    return ready;
}

bool EthercatMaster::waitForSlavesInPreop(
    const StaticSlaveConfig &config,
    std::string &error,
    const std::function<int(std::uint16_t, ec_slave_info_t &)> &query,
    const std::function<void()> &wait,
    std::size_t max_attempts,
    const std::function<bool()> &expired) {
    error.clear();
    std::size_t stable_count = 0;

    // Keep the last-seen info for every slave so the timeout message can report
    // all of them rather than only the final one visited in the scan loop.
    struct PendingEntry {
        std::size_t index;
        ec_slave_info_t info;
    };
    std::vector<PendingEntry> pending(config.slave_count);
    for (std::size_t i = 0; i < config.slave_count; ++i) {
        pending[i].index = i;
        pending[i].info = {};
    }

    bool first_all_ready_seen = false;
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        if (expired && expired()) {
            break;
        }
        bool all_ready = true;
        for (std::size_t slave_index = 0; slave_index < config.slave_count; ++slave_index) {
            const SlaveSpec &slave = config.slaves[slave_index];
            ec_slave_info_t slave_info{};
            if (query(slave.position, slave_info) != 0) {
                error = "failed to read state for slave[" + std::to_string(slave_index) + "]";
                return false;
            }
            pending[slave_index].info = slave_info;
            if (!slaveReadyForConfiguration(slave_info)) {
                all_ready = false;
            }
        }

        if (expired && expired()) {
            break;
        }
        if (all_ready) {
            // Require kPreopStableConfirmations consecutive all-ready polls before
            // proceeding. The IgH kernel module may briefly set error_flag while
            // driving a slave from OP back to PREOP; a single passing sample is
            // not sufficient evidence that the slave is stable enough for PDO
            // remapping.
            ++stable_count;
            if (!first_all_ready_seen) {
                first_all_ready_seen = true;
                std::cout << "[EthercatMaster] All slaves reached PREOP after "
                          << attempt << " attempts, waiting for "
                          << kPreopStableConfirmations << " stable confirmations\n";
            }
            if (stable_count >= kPreopStableConfirmations) {
                std::cout << "[EthercatMaster] PREOP stable confirmations complete\n";
                return true;
            }
        } else {
            if (stable_count > 0) {
                std::cout << "[EthercatMaster] PREOP stability lost at attempt "
                          << attempt << ", resetting stable_count from "
                          << stable_count << " to 0\n";
            }
            stable_count = 0;
            // Log not-ready slaves every 50 attempts during the wait.
            if (attempt % 50U == 0U) {
                std::cout << "[EthercatMaster] Waiting for PREOP (attempt "
                          << attempt << "):";
                for (const PendingEntry &entry : pending) {
                    if (!slaveReadyForConfiguration(entry.info)) {
                        std::cout << " slave[" << entry.index << "]="
                                  << alStateName(entry.info.al_state)
                                  << (entry.info.error_flag ? "(ERR)" : "");
                    }
                }
                std::cout << '\n';
            }
        }

        if (attempt + 1U < max_attempts) {
            wait();
        }
    }

    // Build a message listing every slave that is still not ready.
    error = "timed out waiting for slaves to reach PREOP:";
    for (const PendingEntry &entry : pending) {
        if (!slaveReadyForConfiguration(entry.info)) {
            error += " slave[" + std::to_string(entry.index) +
                     "](al_state=" +
                     std::to_string(static_cast<unsigned int>(entry.info.al_state)) +
                     ",error_flag=" +
                     std::to_string(static_cast<unsigned int>(entry.info.error_flag)) + ")";
        }
    }
    return false;
}

// Receives a frame and processes both domains' working counters (rt_safe).
void EthercatMaster::receiveAndProcess() noexcept {
    if (!initialized_ || master_ == nullptr || input_domain_ == nullptr || output_domain_ == nullptr) {
        return;
    }

    (void)ecrt_master_receive(master_);
    (void)ecrt_domain_process(input_domain_);
    (void)ecrt_domain_process(output_domain_);
}

// Sets application time at a stable point in each realtime cycle.
bool EthercatMaster::setApplicationTime(std::uint64_t app_time_ns) noexcept {
    if (!dc_enabled_) {
        return true;
    }
    if (!initialized_ || master_ == nullptr) {
        return false;
    }
    const int result = ecrt_master_application_time(master_, app_time_ns);
    if (result != 0) {
        recordDcError(DcErrorStage::ApplicationTime, result);
        return false;
    }
    return true;
}

// Re-queues both domains, queues optional DC sync datagrams, and sends (rt_safe).
bool EthercatMaster::queueAndSend() noexcept {
    if (!initialized_ || master_ == nullptr || input_domain_ == nullptr || output_domain_ == nullptr) {
        return false;
    }

    (void)ecrt_domain_queue(input_domain_);
    (void)ecrt_domain_queue(output_domain_);
    if (dc_enabled_) {
        // Keep application_time on the nominal deadline; drift compensation
        // instead uses the actual time close to transmission (dc_user example).
        timespec now{};
        if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            recordDcError(DcErrorStage::SyncReferenceClock, -errno);
            return false;
        }
        const auto sync_time = static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
                               static_cast<std::uint64_t>(now.tv_nsec);
        const int reference_result = ecrt_master_sync_reference_clock_to(master_, sync_time);
        if (reference_result != 0) {
            recordDcError(DcErrorStage::SyncReferenceClock, reference_result);
            return false;
        }
        const int slave_result = ecrt_master_sync_slave_clocks(master_);
        if (slave_result != 0) {
            recordDcError(DcErrorStage::SyncSlaveClocks, slave_result);
            return false;
        }
    }
    (void)ecrt_master_send(master_);
    return true;
}

// Snapshots master and domain state into a value object without allocation.
BusState EthercatMaster::readState() noexcept {
    BusState state{};
    if (!initialized_) {
        return state;
    }

    ec_master_state_t master_state{};
    ec_domain_state_t input_state{};
    ec_domain_state_t output_state{};

    if (ecrt_master_state(master_, &master_state) != 0 ||
        ecrt_domain_state(input_domain_, &input_state) != 0 ||
        ecrt_domain_state(output_domain_, &output_state) != 0) {
        return state;
    }

    state.responding_slaves = master_state.slaves_responding;
    state.al_states = master_state.al_states;
    state.link_up = (master_state.link_up != 0U);
    state.input_working_counter = input_state.working_counter;
    state.output_working_counter = output_state.working_counter;
    state.input_wc_state = input_state.wc_state;
    state.output_wc_state = output_state.wc_state;

    return state;
}

int EthercatMaster::pollSlaves(ec_al_state_t target, bool &all_ready) noexcept {
    all_ready = initialized_ && !slave_configs_.empty();
    int first_error = 0;
    for (const auto *sc : slave_configs_) {
        ec_slave_config_state_t state{};
        const int rc = ecrt_slave_config_state(sc, &state);
        if (rc != 0 && first_error == 0) {
            first_error = rc;
        }
        if (rc != 0 || !state.online || state.al_state != target ||
            (target == EC_AL_STATE_OP && !state.operational)) {
            all_ready = false;
        }
    }
    return first_error;
}

// Returns the cached input-domain process-data base pointer.
std::uint8_t *EthercatMaster::inputData() noexcept {
    return input_data_;
}

// Returns the cached output-domain process-data base pointer.
std::uint8_t *EthercatMaster::outputData() noexcept {
    return output_data_;
}

// Returns the input-domain process-data size in bytes.
std::size_t EthercatMaster::inputSize() const noexcept {
    return input_size_;
}

// Returns the output-domain process-data size in bytes.
std::size_t EthercatMaster::outputSize() const noexcept {
    return output_size_;
}

// Returns true once initialize() has completed successfully.
bool EthercatMaster::initialized() const noexcept {
    return initialized_;
}

DcError EthercatMaster::lastDcError() const noexcept {
    return last_dc_error_;
}

void EthercatMaster::recordDcError(DcErrorStage stage, int error_code) noexcept {
    last_dc_error_.stage = stage;
    last_dc_error_.error_code = error_code;
}

// Releases the master at most once and nulls every borrowed pointer and domain.
void EthercatMaster::reset() noexcept {
    slave_configs_.clear();
    input_data_ = nullptr;
    output_data_ = nullptr;
    input_size_ = 0;
    output_size_ = 0;
    input_domain_ = nullptr;
    output_domain_ = nullptr;
    dc_enabled_ = false;
    initialized_ = false;

    if (master_ != nullptr) {
        ecrt_release_master(master_);
        master_ = nullptr;
    }
}

}  // namespace rocos
