#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>

#include "pdo_config.hpp"

#if ROCOS_IGH_BUILD_MASTER
#include <ecrt.h>
#else
#ifndef __ECRT_H__
struct ec_sync_info;
using ec_sync_info_t = struct ec_sync_info;
#endif
#endif

namespace rocos {

struct EcatBus;

/// @brief Direction of a PDO entry relative to the master.
enum class PdoDirection { Input, Output };

/**
 * @brief Describes one process-data entry within a slave.
 *
 * `offset` and `bit_position` are written back by IgH during PDO registration,
 * so they are mutable output fields.
 */
struct PdoEntrySpec {
    const char *name;            ///< Unique variable name.
    PdoDirection direction;      ///< Input (TxPDO) or output (RxPDO).
    std::uint16_t index;         ///< PDO object index.
    std::uint8_t sub_index;      ///< PDO object sub-index.
    std::uint8_t bit_length;     ///< Bit width (must be a multiple of 8).
    unsigned int offset;         ///< Byte offset in the domain (filled by IgH).
    unsigned int bit_position;   ///< Bit position within the byte (must be 0).
};

/**
 * @brief Compile-time description of one slave and its PDO entries.
 */
struct SlaveSpec {
    std::uint16_t alias;         ///< Slave alias (0 = use position).
    std::uint16_t position;      ///< Bus position, or offset from the alias.
    std::uint32_t vendor_id;     ///< Expected vendor ID, or 0 with product_code 0 to discover it.
    std::uint32_t product_code;  ///< Expected product code, or 0 with vendor_id 0 to discover it.
    const char *name;            ///< Human-readable slave name.
    const ec_sync_info_t *syncs; ///< IgH sync-manager/PDO table (EC_END terminated).
    PdoEntrySpec *entries;       ///< PDO entry descriptors (mutable offsets).
    std::size_t entry_count;     ///< Number of entries in `entries`.
};

/**
 * @brief The complete compile-time slave configuration.
 */
struct StaticSlaveConfig {
    SlaveSpec *slaves;       ///< Slave descriptors.
    std::size_t slave_count; ///< Number of slaves.
};

#if ROCOS_IGH_BUILD_MASTER
/** @brief Owns dynamic IgH configuration storage and exposes stable pointer views. */
class LoadedSlaveConfig {
public:
    LoadedSlaveConfig();
    ~LoadedSlaveConfig();

    LoadedSlaveConfig(const LoadedSlaveConfig &) = delete;
    LoadedSlaveConfig &operator=(const LoadedSlaveConfig &) = delete;
    LoadedSlaveConfig(LoadedSlaveConfig &&) noexcept;
    LoadedSlaveConfig &operator=(LoadedSlaveConfig &&) noexcept;

    bool build(PdoBusConfig config, std::string &error) noexcept;
    StaticSlaveConfig view() noexcept;

private:
    friend void printLoadedSlaveConfig(const LoadedSlaveConfig &, std::ostream &);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void printLoadedSlaveConfig(const LoadedSlaveConfig &config, std::ostream &output);
#endif

/**
 * @brief Applies a non-zero identity read from scanned SII information.
 */
bool applyDiscoveredIdentity(SlaveSpec &slave,
                             std::uint32_t vendor_id,
                             std::uint32_t product_code,
                             std::string &error) noexcept;

/**
 * @brief Validates a static configuration structurally.
 *
 * @param config Configuration to validate (empty is valid).
 * @param error  Receives the first exact failure description.
 * @return True when the configuration is structurally valid.
 */
bool validateSlaveConfig(const StaticSlaveConfig &config, std::string &error) noexcept;

/**
 * @brief Copies the static configuration into shared EcatBus metadata.
 *
 * @param bus    Shared EcatBus to populate (cleared first).
 * @param config Source static configuration.
 * @param error  Receives the first failure description.
 * @return True on success.
 */
bool publishConfig(EcatBus &bus, const StaticSlaveConfig &config, std::string &error) noexcept;

}  // namespace rocos
