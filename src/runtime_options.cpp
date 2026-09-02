#include "runtime_options.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace rocos {
namespace {

constexpr std::uint32_t kMinPeriodUs = 1000U;

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

bool parseRuntimeOptions(int argc, char **argv, RuntimeOptions &options, std::string &error) {
    error.clear();
    options = RuntimeOptions{};

    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (flag == "--help") {
            options.show_help = true;
            continue;
        }
        if (flag != "--master-id" && flag != "--period-us") {
            error = "unknown option: " + std::string(flag);
            return false;
        }

        if (i + 1 >= argc) {
            error = "missing value for option: " + std::string(flag);
            return false;
        }

        const std::string_view value_text(argv[++i]);
        std::uint64_t parsed_value = 0;
        if (!parseUnsigned(value_text, parsed_value)) {
            error = "invalid value for option: " + std::string(flag);
            return false;
        }

        if (flag == "--master-id") {
            if (parsed_value > static_cast<std::uint64_t>(std::numeric_limits<unsigned int>::max())) {
                error = "master-id is out of range";
                return false;
            }
            options.master_id = static_cast<unsigned int>(parsed_value);
            continue;
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
    }

    return true;
}

const char *runtimeOptionsUsage() noexcept {
    return "usage: rocos_igh_master [--master-id <id>] [--period-us <period>] [--help]";
}

}  // namespace rocos
