#include "ethercat_master.hpp"

#include "shared_memory_config.hpp"

#include <chrono>
#include <thread>

namespace rocos {

namespace {

constexpr auto kPreopPollInterval = std::chrono::milliseconds(10);
constexpr std::size_t kPreopMaxAttempts = 500U;

}  // namespace

// Releases the requested IgH master and drops all borrowed pointers.
EthercatMaster::~EthercatMaster() {
    reset();
}

// Requests the IgH master, creates one input and one output domain, configures
// each slave and registers its PDO entries, then activates the master and caches
// both domain buffers and sizes. Fails fast with reset() on any error.
bool EthercatMaster::initialize(unsigned int master_id, StaticSlaveConfig config, std::string &error) {
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

    master_ = ecrt_request_master(master_id);
    if (master_ == nullptr) {
        error = "failed to request EtherCAT master";
        reset();
        return false;
    }

    // IgH requests PREOP asynchronously when the master is reserved. Activating
    // while a slave is still OP can overwrite that request and skip reconfiguration.
    if (!waitForSlavesInPreop(config, error)) {
        reset();
        return false;
    }

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

        std::size_t entry_index = 0;
        for (unsigned int sync_position = 0; sync_position < 2U; ++sync_position) {
            const ec_sync_info_t &sync = slave.syncs[sync_position];
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

    if (ecrt_master_activate(master_) != 0) {
        error = "failed to activate EtherCAT master";
        reset();
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

// Receives a frame and processes both domains' working counters (rt_safe).
void EthercatMaster::receiveAndProcess() noexcept {
    if (!initialized_ || master_ == nullptr || input_domain_ == nullptr || output_domain_ == nullptr) {
        return;
    }

    (void)ecrt_master_receive(master_);
    (void)ecrt_domain_process(input_domain_);
    (void)ecrt_domain_process(output_domain_);
}

// Re-queues both domains and sends all queued datagrams (rt_safe).
void EthercatMaster::queueAndSend() noexcept {
    if (!initialized_ || master_ == nullptr || input_domain_ == nullptr || output_domain_ == nullptr) {
        return;
    }

    (void)ecrt_domain_queue(input_domain_);
    (void)ecrt_domain_queue(output_domain_);
    (void)ecrt_master_send(master_);
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

// Releases the master at most once and nulls every borrowed pointer and domain.
void EthercatMaster::reset() noexcept {
    input_data_ = nullptr;
    output_data_ = nullptr;
    input_size_ = 0;
    output_size_ = 0;
    input_domain_ = nullptr;
    output_domain_ = nullptr;
    initialized_ = false;

    if (master_ != nullptr) {
        ecrt_release_master(master_);
        master_ = nullptr;
    }
}

}  // namespace rocos
