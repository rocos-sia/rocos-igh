#pragma once

#include "pdo_config.hpp"
#include <iosfwd>

#if ROCOS_IGH_BUILD_MASTER
#include <ecrt.h>
#endif

namespace rocos {

// Startup-only formatting: never called by the cyclic task.
void printStartupConfiguration(const PdoBusConfig &config, const RuntimeOptions &effective,
                               const char *version, std::ostream &output, bool color = false);

#if ROCOS_IGH_BUILD_MASTER
// Read the existing IgH scan snapshot; never triggers a scan or changes device state.
// Diagnostic query failures are reported and do not change initialization behavior.
// Returns true only when the snapshot query succeeds and scanning has completed.
bool printBusScan(ec_master_t *master, unsigned int master_id, std::size_t configured,
                  std::ostream &output, bool color = false);
#endif

} // namespace rocos
