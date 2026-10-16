#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "runtime_options.hpp"

namespace rocos {

struct PdoEntryConfig {
    std::string name;
    std::uint16_t index{0};
    std::uint8_t sub_index{0};
    std::uint8_t bit_length{0};
};

struct PdoMappingConfig {
    std::uint16_t index{0};
    std::vector<PdoEntryConfig> entries;
};

struct DistributedClockConfig {
    std::uint16_t assign_activate{0};
    std::int32_t sync0_shift_ns{0};
    std::uint32_t sync1_cycle_ns{0};
    std::int32_t sync1_shift_ns{0};
    bool reference{false};
};

// A validated scalar SDO download, encoded in EtherCAT little-endian wire order.
struct InitCommandConfig {
    std::uint16_t index{0};
    std::uint8_t sub_index{0};
    std::uint8_t size{0};
    std::array<std::uint8_t, 8> data{};
};

struct SlavePdoConfig {
    std::uint16_t id{0};
    std::string name;
    std::vector<PdoMappingConfig> rx_pdos;
    std::vector<PdoMappingConfig> tx_pdos;
    std::optional<DistributedClockConfig> dc;
    std::vector<InitCommandConfig> init_commands;
};

struct PdoBusConfig {
    std::vector<SlavePdoConfig> slaves;
    RuntimeOptions master; ///< Optional YAML startup defaults; CLI overrides these.
    std::string source_yaml; ///< Original file snapshot for guarded discovery writeback.
};

bool loadPdoConfig(const std::string &path,
                   PdoBusConfig &config,
                   std::string &error) noexcept;

// Replace only originally empty entries, preserving surrounding YAML text.
// Validates the result and atomically replaces the file if its snapshot still matches.
bool saveDiscoveredPdoConfig(const std::string &path, const PdoBusConfig &original,
                             PdoBusConfig &resolved, std::string &error) noexcept;

}  // namespace rocos
