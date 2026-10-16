#include "process_data_layout.hpp"
#include "shared_memory_config.hpp"

#include <algorithm>
#include <utility>

namespace rocos {
bool ProcessDataLayout::build(StaticSlaveConfig config, std::size_t input_domain_size,
                              std::size_t output_domain_size, std::string &error) noexcept {
    try {
        error.clear();
        if (config.slave_count > MAX_SLAVE_NUM) {
            error = "slave count exceeds MAX_SLAVE_NUM";
            return false;
        }
        if (!validateSlaveConfig(config, error)) return false;
        if (input_domain_size > EC_SHM_MAX_SIZE || output_domain_size > EC_SHM_MAX_SIZE) {
            error = "domain size exceeds EC_SHM_MAX_SIZE";
            return false;
        }
        ProcessDataLayout candidate;
        candidate.input_size_ = input_domain_size;
        candidate.output_size_ = output_domain_size;
        std::vector<std::pair<PdoEntrySpec *, unsigned int>> offsets;
        for (std::size_t s = 0; s < config.slave_count; ++s) {
            auto &slave = config.slaves[s];
            for (std::size_t e = 0; e < slave.entry_count; ++e) {
                auto &entry = slave.entries[e];
                const bool input = entry.direction == PdoDirection::Input;
                const auto domain_size = input ? input_domain_size : output_domain_size;
                const auto wire_bytes = (entry.bit_position + entry.bit_length + 7U) / 8U;
                if (entry.offset > domain_size || wire_bytes > domain_size - entry.offset) {
                    error = "PDO wire range exceeds domain at slave[" + std::to_string(s) +
                        "] entry[" + std::to_string(e) + "]";
                    return false;
                }
                if (entry.bit_position == 0 && entry.bit_length % 8 == 0) {
                    offsets.emplace_back(&entry, entry.offset);
                    continue;
                }
                auto &shared_size = input ? candidate.input_size_ : candidate.output_size_;
                // Align storage for ordinary integer clients. Its size is
                // ceil(bit_length / 8), independently of the packed wire offset.
                constexpr auto alignment = alignof(std::max_align_t);
                const auto offset = (shared_size + alignment - 1) / alignment * alignment;
                const auto bytes = (entry.bit_length + 7U) / 8U;
                if (offset > EC_SHM_MAX_SIZE || bytes > EC_SHM_MAX_SIZE - offset) {
                    error = "expanded PDO image exceeds EC_SHM_MAX_SIZE";
                    return false;
                }
                auto &transfers = input ? candidate.inputs_ : candidate.outputs_;
                transfers.push_back({entry.offset, offset, entry.bit_position, entry.bit_length});
                offsets.emplace_back(&entry, static_cast<unsigned int>(offset));
                shared_size = offset + bytes;
            }
        }
        for (const auto &offset : offsets) offset.first->shared_offset = offset.second;
        *this = std::move(candidate);
        return true;
    } catch (const std::exception &e) {
        error = std::string("failed to build PDO byte layout: ") + e.what();
        return false;
    }
}

void ProcessDataLayout::unpackInputs(const std::uint8_t *domain, void *shared) const noexcept {
    if (domain == nullptr || shared == nullptr) return;
    auto *target = static_cast<std::uint8_t *>(shared);
    for (const auto &t : inputs_) {
        for (unsigned int byte = 0, remaining = t.bit_length; remaining != 0; ++byte) {
            const auto bits = std::min(remaining, 8U);
            unsigned int value = domain[t.domain_offset + byte] >> t.bit_position;
            if (t.bit_position + bits > 8)
                value |= unsigned(domain[t.domain_offset + byte + 1]) << (8 - t.bit_position);
            target[t.shared_offset + byte] = static_cast<std::uint8_t>(value & ((1U << bits) - 1U));
            remaining -= bits;
        }
    }
}

void ProcessDataLayout::packOutputs(const void *shared, std::uint8_t *domain) const noexcept {
    if (domain == nullptr || shared == nullptr) return;
    const auto *source = static_cast<const std::uint8_t *>(shared);
    for (const auto &t : outputs_) {
        for (unsigned int byte = 0, remaining = t.bit_length; remaining != 0; ++byte) {
            const auto bits = std::min(remaining, 8U);
            const auto value_mask = (1U << bits) - 1U;
            const auto mask = value_mask << t.bit_position;
            const auto value = (unsigned(source[t.shared_offset + byte]) & value_mask) << t.bit_position;
            auto &low = domain[t.domain_offset + byte];
            low = static_cast<std::uint8_t>((low & ~mask) | value);
            if (t.bit_position + bits > 8) {
                auto &high = domain[t.domain_offset + byte + 1];
                high = static_cast<std::uint8_t>((high & ~(mask >> 8)) | (value >> 8));
            }
            remaining -= bits;
        }
    }
}

void ProcessDataLayout::reset() noexcept {
    inputs_.clear(); outputs_.clear();
    input_size_ = output_size_ = 0;
}
} // namespace rocos
