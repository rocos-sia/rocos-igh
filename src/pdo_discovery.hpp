#pragma once

#include "pdo_config.hpp"
#include <chrono>
#include <ecrt.h>

namespace rocos {
// Startup only: SDO uploads of currently installed mappings, before any downloads.
// All empty PDOs must resolve successfully before the original YAML is replaced.
bool discoverEmptyPdoEntries(ec_master_t *master, PdoBusConfig &config,
                              const std::string &path,
                              std::chrono::steady_clock::time_point deadline,
                              std::string &error) noexcept;
} // namespace rocos
