#include "slave_config.hpp"

#include "shared_memory_config.hpp"

#include <string>

namespace rocos {

StaticSlaveConfig defaultSlaveConfig() noexcept {
    return {nullptr, 0};
}

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
            if (slave.vendor_id == 0) {
                error = "slave[" + std::to_string(slave_index) + "] vendor_id must be non-zero";
                return false;
            }
            if (slave.product_code == 0) {
                error = "slave[" + std::to_string(slave_index) + "] product_code must be non-zero";
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

}  // namespace rocos
