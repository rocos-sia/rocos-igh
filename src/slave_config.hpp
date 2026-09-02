#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#if ROCOS_IGH_BUILD_MASTER
#include <ecrt.h>
#else
struct ec_sync_info;
using ec_sync_info_t = struct ec_sync_info;
#endif

namespace rocos {

enum class PdoDirection { Input, Output };

struct PdoEntrySpec {
    const char *name;
    PdoDirection direction;
    std::uint16_t index;
    std::uint8_t sub_index;
    std::uint8_t bit_length;
    unsigned int offset;
    unsigned int bit_position;
};

struct SlaveSpec {
    std::uint16_t alias;
    std::uint16_t position;
    std::uint32_t vendor_id;
    std::uint32_t product_code;
    const char *name;
    const ec_sync_info_t *syncs;
    PdoEntrySpec *entries;
    std::size_t entry_count;
};

struct StaticSlaveConfig {
    SlaveSpec *slaves;
    std::size_t slave_count;
};

StaticSlaveConfig defaultSlaveConfig() noexcept;
bool validateSlaveConfig(const StaticSlaveConfig &config, std::string &error) noexcept;

}  // namespace rocos
