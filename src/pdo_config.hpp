#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

struct SlavePdoConfig {
    std::uint16_t id{0};
    std::string name;
    std::vector<PdoMappingConfig> rx_pdos;
    std::vector<PdoMappingConfig> tx_pdos;
};

struct PdoBusConfig {
    std::vector<SlavePdoConfig> slaves;
};

bool loadPdoConfig(const std::string &path,
                   PdoBusConfig &config,
                   std::string &error) noexcept;

}  // namespace rocos