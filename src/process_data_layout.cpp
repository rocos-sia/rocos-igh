#include "process_data_layout.hpp"
#include "shared_memory_config.hpp"

namespace rocos {
bool ProcessDataLayout::build(StaticSlaveConfig config, std::size_t input_domain_size,
                              std::size_t output_domain_size, std::string &error) noexcept {
    try {
        error.clear();
        if (!validateSlaveConfig(config, error)) return false;
        if (input_domain_size > EC_SHM_MAX_SIZE || output_domain_size > EC_SHM_MAX_SIZE) {
            error = "domain size exceeds EC_SHM_MAX_SIZE";
            return false;
        }
        for (std::size_t s = 0; s < config.slave_count; ++s) {
            const auto &slave = config.slaves[s];
            for (std::size_t e = 0; e < slave.entry_count; ++e) {
                const auto &entry = slave.entries[e];
                if (entry.bit_position != 0 || entry.bit_length % 8 != 0) {
                    error = "ABI v2 requires byte-aligned PDO entries at slave[" +
                        std::to_string(s) + "] entry[" + std::to_string(e) + "]";
                    return false;
                }
                const auto size = entry.direction == PdoDirection::Input ? input_domain_size : output_domain_size;
                if (entry.offset > size || entry.bit_length / 8U > size - entry.offset) {
                    error = "PDO wire range exceeds domain";
                    return false;
                }
            }
        }
        for (std::size_t s = 0; s < config.slave_count; ++s)
            for (std::size_t e = 0; e < config.slaves[s].entry_count; ++e)
                config.slaves[s].entries[e].shared_offset = config.slaves[s].entries[e].offset;
        input_size_ = input_domain_size;
        output_size_ = output_domain_size;
        return true;
    } catch (...) {
        error = "failed to validate PDO byte layout";
        return false;
    }
}

// Kept for source compatibility. The domain and shared byte images now coincide.
void ProcessDataLayout::unpackInputs(const std::uint8_t *, void *) const noexcept {}
void ProcessDataLayout::packOutputs(const void *, std::uint8_t *) const noexcept {}
void ProcessDataLayout::reset() noexcept { input_size_ = output_size_ = 0; }
} // namespace rocos
