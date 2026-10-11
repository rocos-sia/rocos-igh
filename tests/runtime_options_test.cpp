#include "runtime_options.hpp"

#include <iostream>
#include <string>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "line " << __LINE__ << ": " << #condition << '\n'; return false; \
} } while (false)

bool testCpuOptions() {
    char program[] = "rocos_igh_master";
    char config_flag[] = "--config";
    char config[] = "config/talon_pdo.yaml";
    char cpu_flag[] = "--cpu";
    char *defaults[]{program, config_flag, config};
    rocos::RuntimeOptions options;
    std::string error;
    CHECK(rocos::parseRuntimeOptions(3, defaults, options, error));
    CHECK(!options.cpu.has_value());
    CHECK(options.master_id == 0 && options.period_us == 1000);

    for (std::string value : {"0", "3"}) {
        char *args[]{program, config_flag, config, cpu_flag, value.data()};
        CHECK(rocos::parseRuntimeOptions(5, args, options, error));
        CHECK(error.empty());
        CHECK(options.cpu.has_value() && *options.cpu == std::stoul(value));
        char *duplicate[]{program, config_flag, config, cpu_flag, value.data(), cpu_flag, value.data()};
        CHECK(!rocos::parseRuntimeOptions(7, duplicate, options, error));
        CHECK(error.find("duplicate") != std::string::npos);
    }
    for (std::string value : {"", "-1", "+1", "1.5", "3x", "0x3", "4294967296", "18446744073709551616"}) {
        char *args[]{program, config_flag, config, cpu_flag, value.data()};
        CHECK(!rocos::parseRuntimeOptions(5, args, options, error));
        CHECK(!error.empty());
    }
    char *missing[]{program, config_flag, config, cpu_flag};
    CHECK(!rocos::parseRuntimeOptions(4, missing, options, error));
    char dc_flag[] = "--dc";
    char dc_on[] = "on";
    char op_flag[] = "--op-timeout-ms";
    char op_value[] = "60000";
    char cpu_value[] = "3";
    char *full[]{program, config_flag, config, dc_flag, dc_on, op_flag, op_value, cpu_flag, cpu_value};
    CHECK(rocos::parseRuntimeOptions(9, full, options, error));
    CHECK(options.cpu == 3U && options.dc_enabled && options.op_timeout_ms == 60000);
    CHECK(rocos::parseRuntimeOptions(3, defaults, options, error));
    CHECK(!options.cpu.has_value());
    return true;
}

bool testPdoAttempts() {
    char program[] = "master", config_flag[] = "--config", config[] = "pdo.yaml";
    char flag[] = "--pdo-config-attempts";
    rocos::RuntimeOptions options;
    std::string error;
    char *defaults[]{program, config_flag, config};
    CHECK(rocos::parseRuntimeOptions(3, defaults, options, error));
    CHECK(options.pdo_config_attempts == 5);
    for (std::string value : {"1", "5", "8", "4294967295"}) {
        char *args[]{program, config_flag, config, flag, value.data()};
        CHECK(rocos::parseRuntimeOptions(5, args, options, error));
        CHECK(options.pdo_config_attempts == std::stoull(value));
        char *duplicate[]{program, config_flag, config, flag, value.data(), flag, value.data()};
        CHECK(!rocos::parseRuntimeOptions(7, duplicate, options, error));
        CHECK(error.find("duplicate") != std::string::npos);
    }
    for (std::string value : {"0", "-1", "+1", "", "1.5", "5x", "4294967296", "18446744073709551616"}) {
        char *args[]{program, config_flag, config, flag, value.data()};
        CHECK(!rocos::parseRuntimeOptions(5, args, options, error));
    }
    char *missing[]{program, config_flag, config, flag};
    CHECK(!rocos::parseRuntimeOptions(4, missing, options, error));
    CHECK(rocos::parseRuntimeOptions(3, defaults, options, error));
    CHECK(options.pdo_config_attempts == 5);
    CHECK(std::string(rocos::runtimeOptionsUsage()).find(flag) != std::string::npos);
    return true;
}

int main() { return testCpuOptions() && testPdoAttempts() ? 0 : 1; }
