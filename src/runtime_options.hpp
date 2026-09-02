#pragma once

#include <cstdint>
#include <string>

namespace rocos {

/**
 * @brief Parsed command-line options for the master process.
 */
struct RuntimeOptions {
    unsigned int master_id{0};     ///< Non-negative EtherCAT master index.
    std::uint32_t period_us{1000}; ///< Cycle period in microseconds (>= 1000).
    bool show_help{false};         ///< True when --help was requested.
};

/**
 * @brief Parses argv strictly into @p options.
 *
 * Accepts --master-id, --period-us, and --help. Rejects unknown flags, missing
 * values, signs, trailing characters, and integer overflow.
 *
 * @param argc    Argument count.
 * @param argv    Argument vector.
 * @param options Receives the parsed values (reset first).
 * @param error   Receives a description of the first failure.
 * @return True on success.
 */
bool parseRuntimeOptions(int argc, char **argv, RuntimeOptions &options, std::string &error);

/// @brief Returns the one-line usage string.
const char *runtimeOptionsUsage() noexcept;

}  // namespace rocos
