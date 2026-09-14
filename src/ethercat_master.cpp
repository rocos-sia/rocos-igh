#include "ethercat_master.hpp"

#include "shared_memory_config.hpp"

#include <cerrno>
#include <chrono>
#include <ctime>
#include <iostream>
#include <thread>

namespace rocos {

namespace {

constexpr auto kPreopPollInterval = std::chrono::milliseconds(10);
constexpr std::size_t kPreopMaxAttempts = 500U;

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
        return "ecrt_master_sync_reference_clock";
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
                                std::string &error) {
    error.clear();

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
    if (!waitForSlavesInPreop(config, error)) {
        reset();
        return false;
    }

    ec_slave_config_t *reference_clock_config = nullptr;
    for (std::size_t slave_index = 0; slave_index < config.slave_count; ++slave_index) {
        SlaveSpec &slave = config.slaves[slave_index];
        if (slave.vendor_id != 0) {
            continue;
        }

        ec_slave_info_t slave_info{};
        if (ecrt_master_get_slave(master_, slave.position, &slave_info) != 0) {
            error = "failed to read SII identity for slave[" + std::to_string(slave_index) + "]";
            reset();
            return false;
        }
        if (!applyDiscoveredIdentity(slave, slave_info.vendor_id, slave_info.product_code, error)) {
            error += " for slave[" + std::to_string(slave_index) + "]";
            reset();
            return false;
        }
        // Verify the identity was actually written before passing it to IgH.
        if (slave.vendor_id == 0 || slave.product_code == 0) {
            error = "SII identity for slave[" + std::to_string(slave_index) +
                    "] is still zero after discovery";
            reset();
            return false;
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

    const auto read_application_time = [](std::uint64_t &application_time) {
        timespec now{};
        if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return errno;
        }
        application_time = static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
                           static_cast<std::uint64_t>(now.tv_nsec);
        return 0;
    };
    const auto apply_application_time = [this](std::uint64_t application_time) {
        return ecrt_master_application_time(master_, application_time);
    };
    const auto activate_master = [this] {
        return ecrt_master_activate(master_);
    };
    if (!activateWithInitialApplicationTime(
            dc_runtime.enabled, error, read_application_time,
            apply_application_time, activate_master)) {
        const DcError dc_error = last_dc_error_;
        reset();
        last_dc_error_ = dc_error;
        return false;
    }

    input_data_ = ecrt_domain_data(input_domain_);
    output_data_ = ecrt_domain_data(output_domain_);
    input_size_ = ecrt_domain_size(input_domain_);
    output_size_ = ecrt_domain_size(output_domain_);

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

    dc_enabled_ = dc_runtime.enabled;
    initialized_ = true;
    return true;
}

bool EthercatMaster::slaveReadyForConfiguration(const ec_slave_info_t &slave_info) noexcept {
    return slave_info.al_state == EC_AL_STATE_PREOP && slave_info.error_flag == 0U;
}

bool EthercatMaster::waitForSlavesInPreop(const StaticSlaveConfig &config,
                                          std::string &error) {
    const auto query = [this](std::uint16_t position, ec_slave_info_t &slave_info) {
        return ecrt_master_get_slave(master_, position, &slave_info);
    };
    const auto wait = [] { std::this_thread::sleep_for(kPreopPollInterval); };
    return waitForSlavesInPreop(config, error, query, wait, kPreopMaxAttempts);
}

bool EthercatMaster::waitForSlavesInPreop(
    const StaticSlaveConfig &config,
    std::string &error,
    const std::function<int(std::uint16_t, ec_slave_info_t &)> &query,
    const std::function<void()> &wait,
    std::size_t max_attempts) {
    error.clear();
    std::size_t pending_slave = 0;
    ec_slave_info_t pending_info{};

    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        bool all_ready = true;
        for (std::size_t slave_index = 0; slave_index < config.slave_count; ++slave_index) {
            const SlaveSpec &slave = config.slaves[slave_index];
            ec_slave_info_t slave_info{};
            if (query(slave.position, slave_info) != 0) {
                error = "failed to read state for slave[" + std::to_string(slave_index) + "]";
                return false;
            }
            if (!slaveReadyForConfiguration(slave_info)) {
                all_ready = false;
                pending_slave = slave_index;
                pending_info = slave_info;
            }
        }

        if (all_ready) {
            return true;
        }
        if (attempt + 1U < max_attempts) {
            wait();
        }
    }

    error = "timed out waiting for slave[" + std::to_string(pending_slave) +
            "] to reach PREOP (al_state=" +
            std::to_string(static_cast<unsigned int>(pending_info.al_state)) +
            ", error_flag=" +
            std::to_string(static_cast<unsigned int>(pending_info.error_flag)) + ")";
    return false;
}

bool EthercatMaster::activateWithInitialApplicationTime(
    bool dc_enabled,
    std::string &error,
    const std::function<int(std::uint64_t &)> &read_time,
    const std::function<int(std::uint64_t)> &apply_time,
    const std::function<int()> &activate) {
    error.clear();
    last_dc_error_ = DcError{};
    const auto seed_application_time = [&](const char *position) {
        std::uint64_t application_time = 0;
        const int clock_result = read_time(application_time);
        if (clock_result != 0) {
            error = std::string("failed to read CLOCK_MONOTONIC ") + position +
                    " master activation (error " +
                    std::to_string(clock_result) + ")";
            return false;
        }
        const int application_result = apply_time(application_time);
        if (application_result != 0) {
            recordDcError(DcErrorStage::ApplicationTime, application_result);
            error = std::string("failed to set DC application time ") + position +
                    " master activation (error " +
                    std::to_string(application_result) + ")";
            return false;
        }
        return true;
    };

    if (dc_enabled && !seed_application_time("before")) {
        return false;
    }
    if (activate() != 0) {
        error = "failed to activate EtherCAT master";
        return false;
    }
    if (dc_enabled && !seed_application_time("after")) {
        return false;
    }
    return true;
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
        const int reference_result = ecrt_master_sync_reference_clock(master_);
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

    (void)ecrt_master_state(master_, &master_state);
    (void)ecrt_domain_state(input_domain_, &input_state);
    (void)ecrt_domain_state(output_domain_, &output_state);

    state.responding_slaves = master_state.slaves_responding;
    state.al_states = master_state.al_states;
    state.link_up = (master_state.link_up != 0U);
    state.input_working_counter = input_state.working_counter;
    state.output_working_counter = output_state.working_counter;
    state.input_wc_state = input_state.wc_state;
    state.output_wc_state = output_state.wc_state;

    return state;
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
    input_data_ = nullptr;
    output_data_ = nullptr;
    input_size_ = 0;
    output_size_ = 0;
    input_domain_ = nullptr;
    output_domain_ = nullptr;
    dc_enabled_ = false;
    last_dc_error_ = DcError{};
    initialized_ = false;

    if (master_ != nullptr) {
        ecrt_release_master(master_);
        master_ = nullptr;
    }
}

}  // namespace rocos
