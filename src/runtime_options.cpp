#include "runtime_options.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace rocos {
namespace {

constexpr std::uint32_t kMinPeriodUs = 1000U;

// Parses a non-negative decimal integer, rejecting signs and trailing characters.
bool parseUnsigned(std::string_view text, std::uint64_t &value) noexcept {
    if (text.empty()) {
        return false;
    }
    if (text.front() == '+' || text.front() == '-') {
        return false;
    }

    value = 0;
    const char *const begin = text.data();
    const char *const end = begin + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

}  // namespace

// Parses --master-id/--period-us/--help strictly, rejecting unknown flags,
// missing values, signs, and out-of-range integers without any EtherCAT side effect.
bool parseRuntimeOptions(int argc, char **argv, RuntimeOptions &options, std::string &error) {
    error.clear();
    options = RuntimeOptions{};
    bool config_seen = false;
    bool master_id_seen = false;
    bool period_us_seen = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--help") {
            options.show_help = true;
            continue;
        }
        if (flag != "--config" && flag != "--master-id" && flag != "--period-us") {
            error = "unknown option: " + std::string(flag);
            return false;
        }

        if (i + 1 >= argc) {
            error = "missing value for option: " + std::string(flag);
            return false;
        }

        const std::string_view value_text(argv[++i]);
        if (flag == "--config") {
            if (config_seen) {
                error = "duplicate option: --config";
                return false;
            }
            if (value_text.empty() || value_text.rfind("--", 0) == 0) {
                error = "invalid value for option: --config";
                return false;
            }
            options.config_path = std::string(value_text);
            config_seen = true;
            continue;
        }

        std::uint64_t parsed_value = 0;
        if (!parseUnsigned(value_text, parsed_value)) {
            error = "invalid value for option: " + std::string(flag);
            return false;
        }

        if (flag == "--master-id") {
            if (master_id_seen) {
                error = "duplicate option: --master-id";
                return false;
            }
            if (parsed_value > static_cast<std::uint64_t>(std::numeric_limits<unsigned int>::max())) {
                error = "master-id is out of range";
                return false;
            }
            options.master_id = static_cast<unsigned int>(parsed_value);
            master_id_seen = true;
            continue;
        }

        if (period_us_seen) {
            error = "duplicate option: --period-us";
            return false;
        }
        if (parsed_value > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            error = "period-us is out of range";
            return false;
        }
        if (parsed_value < static_cast<std::uint64_t>(kMinPeriodUs)) {
            error = "period-us must be >= 1000";
            return false;
        }
        options.period_us = static_cast<std::uint32_t>(parsed_value);
        period_us_seen = true;
    }

    if (!options.show_help && !config_seen) {
        error = "missing required option: --config";
        return false;
    }

    return true;
}

// Returns the one-line usage string.
const char *runtimeOptionsUsage() noexcept {
    return "usage: rocos_igh_master --config <path> [--master-id <id>] [--period-us <period>] [--help]";
}

}  // namespace rocos
