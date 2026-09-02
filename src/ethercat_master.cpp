#include "ethercat_master.hpp"

#include "shared_memory_config.hpp"

namespace rocos {

EthercatMaster::~EthercatMaster() {
    reset();
}

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

        for (std::size_t entry_index = 0; entry_index < slave.entry_count; ++entry_index) {
            PdoEntrySpec &entry = slave.entries[entry_index];
            ec_domain_t *domain =
                (entry.direction == PdoDirection::Input) ? input_domain_ : output_domain_;

            unsigned int bit_position = 0;
            const int offset = ecrt_slave_config_reg_pdo_entry(
                sc, entry.index, entry.sub_index, domain, &bit_position);
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

void EthercatMaster::receiveAndProcess() noexcept {
    if (!initialized_ || master_ == nullptr || input_domain_ == nullptr || output_domain_ == nullptr) {
        return;
    }

    (void)ecrt_master_receive(master_);
    (void)ecrt_domain_process(input_domain_);
    (void)ecrt_domain_process(output_domain_);
}

void EthercatMaster::queueAndSend() noexcept {
    if (!initialized_ || master_ == nullptr || input_domain_ == nullptr || output_domain_ == nullptr) {
        return;
    }

    (void)ecrt_domain_queue(input_domain_);
    (void)ecrt_domain_queue(output_domain_);
    (void)ecrt_master_send(master_);
}

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

std::uint8_t *EthercatMaster::inputData() noexcept {
    return input_data_;
}

std::uint8_t *EthercatMaster::outputData() noexcept {
    return output_data_;
}

std::size_t EthercatMaster::inputSize() const noexcept {
    return input_size_;
}

std::size_t EthercatMaster::outputSize() const noexcept {
    return output_size_;
}

bool EthercatMaster::initialized() const noexcept {
    return initialized_;
}

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
