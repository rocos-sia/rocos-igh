#include "slave_config.hpp"

#include "shared_memory_config.hpp"

#include <cstring>
#include <limits>
#include <string>

namespace rocos {

namespace {

ec_pdo_entry_info_t drive_rx_entries[] = {
    {0x607A, 0x00, 32},
    {0x60FE, 0x00, 32},
    {0x6040, 0x00, 16},
};

ec_pdo_entry_info_t drive_tx_entries[] = {
    {0x6064, 0x00, 32},
    {0x60FD, 0x00, 32},
    {0x6041, 0x00, 16},
};

ec_pdo_info_t drive_rx_pdos[] = {
    {0x1600, 3, drive_rx_entries},
};

ec_pdo_info_t drive_tx_pdos[] = {
    {0x1A00, 3, drive_tx_entries},
};

ec_sync_info_t drive_syncs[] = {
    {2, EC_DIR_OUTPUT, 1, drive_rx_pdos, EC_WD_ENABLE},
    {3, EC_DIR_INPUT, 1, drive_tx_pdos, EC_WD_DISABLE},
    {0xff},
};

PdoEntrySpec drive_entries[] = {
    {"Target Position", PdoDirection::Output, 0x607A, 0x00, 32, 0, 0},
    {"Digital Outputs", PdoDirection::Output, 0x60FE, 0x00, 32, 0, 0},
    {"Control Word", PdoDirection::Output, 0x6040, 0x00, 16, 0, 0},
    {"Position Actual Value", PdoDirection::Input, 0x6064, 0x00, 32, 0, 0},
    {"Digital Inputs", PdoDirection::Input, 0x60FD, 0x00, 32, 0, 0},
    {"Status Word", PdoDirection::Input, 0x6041, 0x00, 16, 0, 0},
};

SlaveSpec drive_slaves[] = {
    {0, 0, 0, 0, "EtherCAT Drive", drive_syncs, drive_entries,
     sizeof(drive_entries) / sizeof(drive_entries[0])},
};

}  // namespace

StaticSlaveConfig defaultSlaveConfig() noexcept {
    return {drive_slaves, sizeof(drive_slaves) / sizeof(drive_slaves[0])};
}

bool applyDiscoveredIdentity(SlaveSpec &slave,
                             std::uint32_t vendor_id,
                             std::uint32_t product_code,
                             std::string &error) noexcept {
    error.clear();
    if (vendor_id == 0 || product_code == 0) {
        error = "discovered SII identity is invalid";
        return false;
    }

    slave.vendor_id = vendor_id;
    slave.product_code = product_code;
    return true;
}

// Validates a static configuration structurally and returns the first exact
// failure in `error` (empty config is valid).
bool validateSlaveConfig(const StaticSlaveConfig &config, std::string &error) noexcept {
    try {
        error.clear();

        if (config.slave_count == 0) {
            return true;
        }
        if (config.slaves == nullptr) {
            error = "config has slave_count > 0 but slaves is null";
            return false;
        }

        for (std::size_t slave_index = 0; slave_index < config.slave_count; ++slave_index) {
            const SlaveSpec &slave = config.slaves[slave_index];
            if ((slave.vendor_id == 0) != (slave.product_code == 0)) {
                error = "slave[" + std::to_string(slave_index) + "] vendor_id and product_code must be both zero or both non-zero";
                return false;
            }
            if (slave.vendor_id == 0 && slave.alias != 0) {
                error = "slave[" + std::to_string(slave_index) + "] SII identity discovery requires alias 0";
                return false;
            }
            if (slave.name == nullptr || slave.name[0] == '\0') {
                error = "slave[" + std::to_string(slave_index) + "] name must be non-empty";
                return false;
            }
            if (slave.syncs == nullptr) {
                error = "slave[" + std::to_string(slave_index) + "] syncs must be non-null";
                return false;
            }
            if (slave.entry_count > 0 && slave.entries == nullptr) {
                error = "slave[" + std::to_string(slave_index) + "] entries must be non-null when entry_count > 0";
                return false;
            }

            std::size_t input_count = 0;
            std::size_t output_count = 0;
            for (std::size_t entry_index = 0; entry_index < slave.entry_count; ++entry_index) {
                const PdoEntrySpec &entry = slave.entries[entry_index];
                if (entry.name == nullptr || entry.name[0] == '\0') {
                    error = "slave[" + std::to_string(slave_index) + "] entry[" + std::to_string(entry_index) + "] name must be non-empty";
                    return false;
                }
                if (entry.bit_length == 0) {
                    error = "slave[" + std::to_string(slave_index) + "] entry[" + std::to_string(entry_index) + "] bit_length must be > 0";
                    return false;
                }
                if ((entry.bit_length % 8U) != 0U) {
                    error = "slave[" + std::to_string(slave_index) + "] entry[" + std::to_string(entry_index) + "] must be byte-aligned (bit_length % 8 == 0)";
                    return false;
                }
                if (entry.bit_position != 0U) {
                    error = "slave[" + std::to_string(slave_index) + "] entry[" + std::to_string(entry_index) + "] bit_position must be 0";
                    return false;
                }

                if (entry.direction == PdoDirection::Input) {
                    ++input_count;
                    if (input_count > MAX_PDINPUT_NUM) {
                        error = "slave[" + std::to_string(slave_index) + "] input entries exceed MAX_PDINPUT_NUM";
                        return false;
                    }
                } else {
                    ++output_count;
                    if (output_count > MAX_PDOUTPUT_NUM) {
                        error = "slave[" + std::to_string(slave_index) + "] output entries exceed MAX_PDOUTPUT_NUM";
                        return false;
                    }
                }
            }
        }

        return true;
    } catch (...) {
        error = "validateSlaveConfig encountered an unexpected exception";
        return false;
    }
}

// Copies the static configuration into the shared EcatBus metadata (slave and
// PDO variable names, offsets, sizes, indices) for client processes to read.
bool publishConfig(EcatBus &bus, const StaticSlaveConfig &config, std::string &error) noexcept {
    try {
        error.clear();
        bus = EcatBus{};

        if (!validateSlaveConfig(config, error)) {
            return false;
        }

        if (config.slave_count > MAX_SLAVE_NUM) {
            error = "slave count exceeds MAX_SLAVE_NUM";
            return false;
        }

        bus.slave_num = static_cast<int>(config.slave_count);
        for (std::size_t slave_index = 0; slave_index < config.slave_count; ++slave_index) {
            const SlaveSpec &source_slave = config.slaves[slave_index];
            Slave &target_slave = bus.slaves[slave_index];

            target_slave.id = static_cast<int>(slave_index);
            std::strncpy(target_slave.name, source_slave.name, MAX_SLAVE_NAME_LEN - 1);
            target_slave.name[MAX_SLAVE_NAME_LEN - 1] = '\0';

            int input_count = 0;
            int output_count = 0;
            for (std::size_t entry_index = 0; entry_index < source_slave.entry_count; ++entry_index) {
                const PdoEntrySpec &entry = source_slave.entries[entry_index];
                if (entry.offset > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
                    error = "entry offset exceeds PdVar range";
                    return false;
                }

                const unsigned int byte_size = static_cast<unsigned int>(entry.bit_length / 8U);
                if (byte_size > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
                    error = "entry size exceeds PdVar range";
                    return false;
                }
                if (entry.offset > EC_SHM_MAX_SIZE) {
                    error = "entry offset exceeds EC_SHM_MAX_SIZE";
                    return false;
                }
                if (byte_size > (EC_SHM_MAX_SIZE - entry.offset)) {
                    error = "entry offset + size exceeds EC_SHM_MAX_SIZE";
                    return false;
                }

                PdVar *target_var = nullptr;
                if (entry.direction == PdoDirection::Input) {
                    if (input_count >= MAX_PDINPUT_NUM) {
                        error = "input entries exceed MAX_PDINPUT_NUM";
                        return false;
                    }
                    target_var = &target_slave.input_vars[input_count++];
                } else {
                    if (output_count >= MAX_PDOUTPUT_NUM) {
                        error = "output entries exceed MAX_PDOUTPUT_NUM";
                        return false;
                    }
                    target_var = &target_slave.output_vars[output_count++];
                }

                std::strncpy(target_var->name, entry.name, MAX_PD_NAME_LEN - 1);
                target_var->name[MAX_PD_NAME_LEN - 1] = '\0';
                target_var->offset = static_cast<int>(entry.offset);
                target_var->size = static_cast<int>(byte_size);
                target_var->index = entry.index;
                target_var->sub_index = entry.sub_index;
            }

            target_slave.input_var_num = input_count;
            target_slave.output_var_num = output_count;
        }

        return true;
    } catch (...) {
        error = "publishConfig encountered an unexpected exception";
        return false;
    }
}

}  // namespace rocos
