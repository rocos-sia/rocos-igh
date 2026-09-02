#pragma once

#include <cstdint>
#include <string>

namespace rocos {

struct RuntimeOptions {
    unsigned int master_id{0};
    std::uint32_t period_us{1000};
    bool show_help{false};
};

bool parseRuntimeOptions(int argc, char **argv, RuntimeOptions &options, std::string &error);
const char *runtimeOptionsUsage() noexcept;

}  // namespace rocos
