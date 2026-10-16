#include "pdo_config.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; \
        return false; \
    } \
} while (false)

namespace {

class TemporaryYamlFile {
public:
    explicit TemporaryYamlFile(const std::string &content) {
        char path_template[] = "/tmp/rocos_igh_pdo_XXXXXX";
        const int fd = mkstemp(path_template);
        if (fd >= 0) {
            close(fd);
            path_ = path_template;
            std::ofstream output(path_);
            output << content;
        }
    }

    ~TemporaryYamlFile() {
        if (!path_.empty()) {
            std::remove(path_.c_str());
        }
    }

    const std::string &path() const noexcept { return path_; }

private:
    std::string path_;
};

std::string validYaml() {
    return R"yaml(version: 1
slaves:
  - id: 0
    name: Drive
    rx_pdos:
      - index: 0x1600
        entries:
          - {name: Control Word, index: 0x6040, sub_index: 0, bit_length: 16}
    tx_pdos:
      - index: 0x1A00
        entries:
          - {name: Status Word, index: 0x6041, sub_index: 0, bit_length: 16}
)yaml";
}

std::string replaceOnce(std::string source,
                        const std::string &needle,
                        const std::string &replacement) {
    const std::size_t position = source.find(needle);
    if (position != std::string::npos) {
        source.replace(position, needle.size(), replacement);
    }
    return source;
}

std::string validDcYaml() {
    return replaceOnce(validYaml(), "    rx_pdos:\n", "    dc_ref: true\n    rx_pdos:\n");
}

bool expectConfigError(const std::string &yaml, const std::string &error_fragment) {
    const TemporaryYamlFile file(yaml);
    rocos::PdoBusConfig config;
    std::string error;
    CHECK(!rocos::loadPdoConfig(file.path(), config, error));
    CHECK(config.slaves.empty());
    CHECK(error.find(file.path()) != std::string::npos);
    CHECK(error.find(error_fragment) != std::string::npos);
    return true;
}

bool testInitCommands() {
    struct Case { const char *type; const char *value; std::uint8_t size; std::uint64_t encoded; };
    const Case cases[]{
        {"uint8", "255", 1, 0xff}, {"int8", "-128", 1, 0x80}, {"int8", "127", 1, 0x7f},
        {"uint16", "65535", 2, 0xffff}, {"int16", "-32768", 2, 0x8000},
        {"int16", "32767", 2, 0x7fff}, {"uint32", "0x89ABCDEF", 4, 0x89abcdef},
        {"int32", "-2147483648", 4, 0x80000000}, {"int32", "2147483647", 4, 0x7fffffff},
        {"uint64", "18446744073709551615", 8, UINT64_MAX},
        {"int64", "-9223372036854775808", 8, UINT64_C(0x8000000000000000)},
        {"int64", "9223372036854775807", 8, UINT64_C(0x7fffffffffffffff)},
        {"int16", "-2", 2, 0xfffe}, {"int32", "0X12345678", 4, 0x12345678},
    };
    for (const auto &c : cases) {
        const auto command = "    init_commands:\n      - {name: Parameter, index: 0x2000, sub_index: 255, Type: " +
                             std::string(c.type) + ", Value: " + c.value + "}\n";
        const TemporaryYamlFile file(replaceOnce(validYaml(), "    rx_pdos:\n", command + "    rx_pdos:\n"));
        rocos::PdoBusConfig config;
        std::string error;
        CHECK(rocos::loadPdoConfig(file.path(), config, error));
        CHECK(config.slaves[0].init_commands.size() == 1);
        const auto &cmd = config.slaves[0].init_commands[0];
        CHECK(cmd.index == 0x2000 && cmd.sub_index == 255 && cmd.size == c.size);
        for (unsigned int i = 0; i < c.size; ++i)
            CHECK(cmd.data[i] == static_cast<std::uint8_t>(c.encoded >> (i * 8)));
    }
    const auto commands = R"yaml(    init_commands:
      - {index: 0x2000, sub_index: 0, type: uint8, value: 1}
      - {index: 0x2000, sub_index: 0, type: uint8, value: 2}
    rx_pdos:
)yaml";
    rocos::PdoBusConfig config;
    std::string error;
    const TemporaryYamlFile ordered(replaceOnce(validYaml(), "    rx_pdos:\n", commands));
    CHECK(rocos::loadPdoConfig(ordered.path(), config, error));
    CHECK(config.slaves[0].init_commands.size() == 2);
    CHECK(config.slaves[0].init_commands[0].data[0] == 1 && config.slaves[0].init_commands[1].data[0] == 2);
    for (const auto &block : {"    rx_pdos:\n", "    init_commands: []\n    rx_pdos:\n"}) {
        const TemporaryYamlFile empty(replaceOnce(validYaml(), "    rx_pdos:\n", block));
        CHECK(rocos::loadPdoConfig(empty.path(), config, error));
        CHECK(config.slaves[0].init_commands.empty());
    }
    return true;
}

bool testInvalidInitCommands() {
    const std::string command = "    init_commands:\n      - {index: 0x2000, sub_index: 0, type: uint8, value: 1}\n    rx_pdos:\n";
    const auto yaml = replaceOnce(validYaml(), "    rx_pdos:\n", command);
    for (const auto &type : {"uint7", "int24", "float32", "bool", "[]", "null"})
        CHECK(expectConfigError(replaceOnce(yaml, "type: uint8", std::string("type: ") + type), "init_commands[0].type"));
    for (const auto &value : {"256", "-1", "1.5", "true", "null", "[]", "{}", "\"1\"", "18446744073709551616"})
        CHECK(expectConfigError(replaceOnce(yaml, "value: 1", std::string("value: ") + value), "init_commands[0].value"));
    for (const auto &type : {"int8", "int16", "int32", "int64"}) {
        const auto signed_yaml = replaceOnce(yaml, "type: uint8", std::string("type: ") + type);
        CHECK(expectConfigError(replaceOnce(signed_yaml, "value: 1", "value: 9223372036854775808"), "init_commands[0].value"));
        CHECK(expectConfigError(replaceOnce(signed_yaml, "value: 1", "value: -9223372036854775809"), "init_commands[0].value"));
    }
    for (const auto &c : std::vector<std::pair<std::string,std::string>>{
             {"int8", "128"}, {"int8", "-129"}, {"int16", "32768"}, {"int16", "-32769"},
             {"int32", "2147483648"}, {"int32", "-2147483649"},
             {"uint16", "65536"}, {"uint32", "4294967296"}, {"int8", "0xff"}}) {
        CHECK(expectConfigError(replaceOnce(replaceOnce(yaml, "type: uint8", "type: " + c.first),
                                            "value: 1", "value: " + c.second), "init_commands[0].value"));
    }
    CHECK(expectConfigError(replaceOnce(yaml, "type: uint8", "type: int8, Type: int8"), "duplicated"));
    CHECK(expectConfigError(replaceOnce(yaml, "value: 1", "value: 1, Value: 1"), "duplicated"));
    CHECK(expectConfigError(replaceOnce(yaml, "value: 1", "value: 1, value: 2"), "duplicated"));
    CHECK(expectConfigError(replaceOnce(yaml, "index: 0x2000", "index: 65536"), "init_commands[0].index"));
    CHECK(expectConfigError(replaceOnce(yaml, "sub_index: 0", "sub_index: 256"), "init_commands[0].sub_index"));
    for (const auto &field : {"index: 0x2000, ", "sub_index: 0, ", "type: uint8, ", ", value: 1"})
        CHECK(expectConfigError(replaceOnce(yaml, field, ""), "is required"));
    CHECK(expectConfigError(replaceOnce(yaml, "value: 1", "value: 1, bit_length: 8"), "is not allowed"));
    for (const auto &block : {"null", "{}", "true", "[1]"}) {
        CHECK(expectConfigError(replaceOnce(validYaml(), "    rx_pdos:\n",
            "    init_commands: " + std::string(block) + "\n    rx_pdos:\n"), "init_commands"));
    }
    return true;
}

bool testMasterOptions() {
    const TemporaryYamlFile file(validYaml() + R"yaml(
master:
  master_id: 2
  period_us: 2000
  cpu: 3
  dc: "on"
  preop_timeout_ms: 30000
  op_timeout_ms: 90000
  pdo_config_attempts: 8
)yaml");
    rocos::PdoBusConfig config;
    rocos::RuntimeOptions options;
    std::string error;
    CHECK(rocos::loadPdoConfig(file.path(), config, error));
    char program[] = "master", flag[] = "--config";
    std::string path = file.path();
    char *args[]{program, flag, path.data()};
    CHECK(rocos::parseRuntimeOptions(3, args, options, error, config.master));
    CHECK(options.config_path == path && !options.show_help);
    CHECK(options.master_id == 2 && options.period_us == 2000 && options.cpu == 3);
    CHECK(options.dc_enabled && options.preop_timeout_ms == 30000);
    CHECK(options.op_timeout_ms == 90000 && options.pdo_config_attempts == 8);

    char period[] = "--period-us", period_value[] = "1000";
    char dc[] = "--dc", off[] = "off", cpu[] = "--cpu", cpu_value[] = "0";
    char *partial[]{program, period, period_value, flag, path.data(), dc, off, cpu, cpu_value};
    CHECK(rocos::parseRuntimeOptions(9, partial, options, error, config.master));
    CHECK(options.period_us == 1000 && !options.dc_enabled && options.cpu == 0);
    CHECK(options.master_id == 2 && options.preop_timeout_ms == 30000);
    CHECK(options.op_timeout_ms == 90000 && options.pdo_config_attempts == 8);
    // Explicit values equal to built-in defaults still override YAML values.
    char master[] = "--master-id", zero[] = "0", preop[] = "--preop-timeout-ms", preop_value[] = "10000";
    char op[] = "--op-timeout-ms", op_value[] = "60000", attempts[] = "--pdo-config-attempts", five[] = "5";
    char *remaining[]{program, flag, path.data(), master, zero, preop, preop_value,
                      op, op_value, attempts, five};
    CHECK(rocos::parseRuntimeOptions(11, remaining, options, error, config.master));
    CHECK(options.master_id == 0 && options.preop_timeout_ms == 10000);
    CHECK(options.op_timeout_ms == 60000 && options.pdo_config_attempts == 5);
    CHECK(options.period_us == 2000 && options.cpu == 3 && options.dc_enabled);
    CHECK(config.master.master_id == 2 && config.master.cpu == 3); // No mutation of YAML defaults.

    char automatic[] = "auto";
    char *unbind[]{program, flag, path.data(), cpu, automatic};
    CHECK(rocos::parseRuntimeOptions(5, unbind, options, error, config.master));
    CHECK(!options.cpu.has_value());
    char *duplicate[]{program, flag, path.data(), cpu, automatic, cpu, cpu_value};
    CHECK(!rocos::parseRuntimeOptions(7, duplicate, options, error, config.master));
    CHECK(error.find("duplicate") != std::string::npos);

    // Validate the combined DC and period, allowing a CLI override to fix it.
    const TemporaryYamlFile large_period(validYaml() + "master: {dc: on, period_us: 4294968}\n");
    CHECK(rocos::loadPdoConfig(large_period.path(), config, error));
    CHECK(!rocos::parseRuntimeOptions(3, args, options, error, config.master));
    CHECK(error.find("DC SYNC0") != std::string::npos);
    CHECK(rocos::parseRuntimeOptions(9, partial, options, error, config.master));
    char *dc_off[]{program, flag, path.data(), dc, off};
    CHECK(rocos::parseRuntimeOptions(5, dc_off, options, error, config.master));
    CHECK(options.period_us == 4294968 && !options.dc_enabled);
    const TemporaryYamlFile long_off(validYaml() + "master: {dc: off, period_us: 4294968}\n");
    CHECK(rocos::loadPdoConfig(long_off.path(), config, error));
    char on[] = "on";
    char *dc_on[]{program, flag, path.data(), dc, on};
    CHECK(!rocos::parseRuntimeOptions(5, dc_on, options, error, config.master));

    // Absent blocks, empty blocks and explicit null CPU all retain defaults.
    for (const std::string &block : {"", "master: {}\n", "master: {cpu: null}\n"}) {
        const TemporaryYamlFile defaults(validYaml() + block);
        CHECK(rocos::loadPdoConfig(defaults.path(), config, error));
        CHECK(rocos::parseRuntimeOptions(3, args, options, error, config.master));
        CHECK(options.master_id == 0 && options.period_us == 1000 && !options.cpu);
        CHECK(!options.dc_enabled && options.preop_timeout_ms == 10000);
        CHECK(options.op_timeout_ms == 60000 && options.pdo_config_attempts == 5);
    }
    CHECK(rocos::loadPdoConfig(ROCOS_IGH_SOURCE_DIR "/config/talon_pdo.yaml", config, error));
    CHECK(config.master.period_us == 1000 && config.master.pdo_config_attempts == 5);
    CHECK(!config.slaves.empty() && config.slaves[0].rx_pdos[0].index == 0x1607);
    return true;
}

bool testInvalidMasterOptions() {
    for (const std::string &key : {"master_id", "period_us", "cpu", "preop_timeout_ms",
                                   "op_timeout_ms", "pdo_config_attempts"}) {
        for (const std::string &value : {"-1", "+1", "1.5", "true", "[]", "{}", "\"1000\"",
                                         "4294967296", "18446744073709551616"}) {
            CHECK(expectConfigError(validYaml() + "master: {" + key + ": " + value + "}\n",
                                    "master." + key));
        }
        CHECK(expectConfigError(validYaml() + "master: {" + key + ": 1000, " + key + ": 2000}\n",
                                "master." + key + " is duplicated"));
        if (key != "cpu") {
            CHECK(expectConfigError(validYaml() + "master: {" + key + ": null}\n", "master." + key));
        }
    }
    for (const std::string &key : {"period_us", "preop_timeout_ms", "op_timeout_ms", "pdo_config_attempts"}) {
        CHECK(expectConfigError(validYaml() + "master: {" + key + ": 0}\n", "master." + key));
    }
    CHECK(expectConfigError(validYaml() + "master: {period_us: 999}\n", "master.period_us"));
    for (const std::string &value : {"true", "false", "1", "null", "[]", "enabled"}) {
        CHECK(expectConfigError(validYaml() + "master: {dc: " + value + "}\n", "master.dc"));
    }
    for (const std::string &key : {"unknown", "period-us", "config", "help"}) {
        CHECK(expectConfigError(validYaml() + "master: {" + key + ": 1}\n", "master." + key));
    }
    CHECK(expectConfigError(validYaml() + "master: {dc: on, dc: off}\n", "master.dc is duplicated"));
    CHECK(expectConfigError(validYaml() + "master: {}\nmaster: {}\n", "root.master is duplicated"));
    for (const std::string &value : {"null", "[]", "true", "1000"}) {
        CHECK(expectConfigError(validYaml() + "master: " + value + "\n", "master must be a mapping"));
    }
    return true;
}

bool testLoadsValidMultiSlaveConfig() {
    const TemporaryYamlFile file(R"yaml(
version: 1
slaves:
  - id: 0
    name: First Drive
    rx_pdos:
      - index: 0x1600
        entries:
          - {name: Target Position, index: 0x607A, sub_index: 0, bit_length: 32}
      - index: 5633
        entries:
          - {name: Control Word, index: 24640, sub_index: 0, bit_length: 16}
    tx_pdos:
      - index: 0x1A00
        entries:
          - {name: Actual Position, index: 0x6064, sub_index: 0, bit_length: 32}
  - id: 1
    name: Second Drive
    rx_pdos:
      - index: 0x1600
        entries:
          - {name: Control Word, index: 0x6040, sub_index: 0, bit_length: 16}
    tx_pdos:
      - index: 0x1A00
        entries:
          - {name: Status Word, index: 0x6041, sub_index: 0, bit_length: 16}
)yaml");

    rocos::PdoBusConfig config;
    std::string error;
    CHECK(rocos::loadPdoConfig(file.path(), config, error));
    CHECK(error.empty());
    CHECK(config.slaves.size() == 2U);
    CHECK(config.slaves[0].id == 0U);
    CHECK(config.slaves[0].name == "First Drive");
    CHECK(config.slaves[0].rx_pdos.size() == 2U);
    CHECK(config.slaves[0].rx_pdos[0].index == 0x1600U);
    CHECK(config.slaves[0].rx_pdos[1].index == 5633U);
    CHECK(config.slaves[0].rx_pdos[0].entries[0].index == 0x607AU);
    CHECK(config.slaves[0].rx_pdos[0].entries[0].bit_length == 32U);
    CHECK(config.slaves[0].tx_pdos[0].entries[0].sub_index == 0U);
    CHECK(config.slaves[1].id == 1U);
    CHECK(config.slaves[1].tx_pdos[0].entries[0].name == "Status Word");
    return true;
}

bool testLoadsDistributedClockConfig() {
    rocos::PdoBusConfig config;
    std::string error;
    for (const std::string &field : {"    dc_ref: true\n", "    dc_ref: false\n", ""}) {
        const TemporaryYamlFile file(replaceOnce(validYaml(), "    rx_pdos:\n", field + "    rx_pdos:\n"));
        CHECK(rocos::loadPdoConfig(file.path(), config, error));
        CHECK(config.slaves.size() == 1 && config.slaves[0].dc.has_value());
        const auto &dc = *config.slaves[0].dc;
        CHECK(dc.assign_activate == 0x0300U && dc.sync0_shift_ns == 0);
        CHECK(dc.sync1_cycle_ns == 0 && dc.sync1_shift_ns == 0);
        CHECK(dc.reference == (field == "    dc_ref: true\n"));
    }
    // A non-first reference is valid; omitted dc_ref still supplies DC defaults.
    const auto second = validDcYaml().substr(validDcYaml().find("  - id:"));
    const TemporaryYamlFile multi(validYaml() + replaceOnce(second, "id: 0", "id: 1"));
    CHECK(rocos::loadPdoConfig(multi.path(), config, error));
    CHECK(config.slaves.size() == 2);
    CHECK(config.slaves[0].dc.has_value() && !config.slaves[0].dc->reference);
    CHECK(config.slaves[1].dc.has_value() && config.slaves[1].dc->reference);
    return true;
}

bool testRejectsInvalidDistributedClockConfig() {
    for (const std::string &value : {"yes", "on", "1", "\"true\"", "null", "[]", "{}"}) {
        CHECK(expectConfigError(replaceOnce(validDcYaml(), "dc_ref: true", "dc_ref: " + value),
                                "dc_ref must be a boolean"));
    }
    CHECK(expectConfigError(replaceOnce(validDcYaml(), "    dc_ref: true\n",
          "    dc_ref: true\n    dc_ref: false\n"), "dc_ref is duplicated"));
    // Removed settings must not be silently accepted and ignored.
    for (const std::string &field : {"dc: {assign_activate: 0x0300, reference: true}",
             "assign_activate: 0x0300", "sync0_shift_ns: 0", "sync1_cycle_ns: 0", "sync1_shift_ns: 0"}) {
        CHECK(expectConfigError(replaceOnce(validDcYaml(), "    dc_ref: true\n",
                                            "    " + field + "\n"), "is not allowed"));
    }
    const auto second = validDcYaml().substr(validDcYaml().find("  - id:"));
    CHECK(expectConfigError(validDcYaml() + replaceOnce(second, "id: 0", "id: 1"),
                            "only one DC reference"));
    return true;
}

  bool testEmptyEntries() {
    const std::string rx_entries =
        "entries:\n          - {name: Control Word, index: 0x6040, sub_index: 0, bit_length: 16}";
    const std::string tx_entries =
        "entries:\n          - {name: Status Word, index: 0x6041, sub_index: 0, bit_length: 16}";
    for (const std::string value : {"[]", "{}", "null", "", "~"}) {
      for (const bool empty_tx : {false, true}) {
        std::string yaml = replaceOnce(validYaml(), rx_entries, "entries: " + value);
        if (empty_tx) yaml = replaceOnce(yaml, tx_entries, "entries: " + value);
        const TemporaryYamlFile file(yaml);
        rocos::PdoBusConfig config;
        std::string error;
        CHECK(rocos::loadPdoConfig(file.path(), config, error));
        CHECK(error.empty());
        CHECK(config.slaves.size() == 1U);
        CHECK(config.slaves[0].rx_pdos.size() == 1U);
        CHECK(config.slaves[0].rx_pdos[0].index == 0x1600);
        CHECK(config.slaves[0].rx_pdos[0].entries.empty());
        CHECK(config.slaves[0].tx_pdos.size() == 1U);
        CHECK(config.slaves[0].tx_pdos[0].index == 0x1A00);
        CHECK(config.slaves[0].tx_pdos[0].entries.size() == (empty_tx ? 0U : 1U));
      }
    }
    for (const std::string value : {"{name: Wrong}", "''", "0", "false", "[null]"}) {
      CHECK(expectConfigError(replaceOnce(validYaml(), rx_entries, "entries: " + value),
                              "slaves[0].rx_pdos[0].entries"));
      CHECK(expectConfigError(replaceOnce(validYaml(), tx_entries, "entries: " + value),
                              "slaves[0].tx_pdos[0].entries"));
    }
    CHECK(expectConfigError(replaceOnce(validYaml(), rx_entries, ""),
                            "slaves[0].rx_pdos[0].entries is required"));
    return true;
  }

  bool testRejectsInvalidConfigFields() {
    std::vector<std::pair<std::string, std::string>> cases;
    cases.emplace_back(validYaml() + "unexpected: true\n", "root.unexpected is not allowed");
    cases.emplace_back(replaceOnce(validYaml(), "version: 1", "version: 2"),
               "version must be 1");
    cases.emplace_back(replaceOnce(validYaml(), "version: 1\n", ""),
               "root.version is required");
    cases.emplace_back(replaceOnce(validYaml(), "id: 0", "id: 1"),
               "slaves[0].id");
    cases.emplace_back(replaceOnce(validYaml(), "id: 0", "id: \"0\""),
               "slaves[0].id must be an unsigned integer");
    cases.emplace_back(replaceOnce(validYaml(), "id: 0", "id: +0"),
               "slaves[0].id must be an unsigned integer");
    cases.emplace_back(replaceOnce(validYaml(), "id: 0", "id: \" 0\""),
               "slaves[0].id must be an unsigned integer");
    cases.emplace_back(replaceOnce(validYaml(), "name: Drive", "id: 0\n    name: Drive"),
               "slaves[0].id is duplicated");
    cases.emplace_back(replaceOnce(validYaml(), "name: Drive", "name: ''"),
               "slaves[0].name");
    cases.emplace_back(replaceOnce(validYaml(), "bit_length: 16", "bit_length: 0"),
               "slaves[0].rx_pdos[0].entries[0].bit_length");
    cases.emplace_back(replaceOnce(validYaml(), "index: 0x6040", "index: 65536"),
               "slaves[0].rx_pdos[0].entries[0].index");
    cases.emplace_back(replaceOnce(validYaml(), "name: Drive", "name: " + std::string(80, 'x')),
               "slaves[0].name");
    cases.emplace_back(replaceOnce(validYaml(), "bit_length: 16}",
                     "bit_length: 16, typo: 1}"),
               "slaves[0].rx_pdos[0].entries[0].typo is not allowed");
    cases.emplace_back(replaceOnce(
                 validYaml(),
                 "rx_pdos:\n      - index: 0x1600\n        entries:\n"
                 "          - {name: Control Word, index: 0x6040, sub_index: 0, bit_length: 16}\n",
                 "rx_pdos: []\n"),
               "slaves[0].rx_pdos");
      cases.emplace_back("version: [\n", "yaml-cpp");

    for (const auto &test_case : cases) {
      CHECK(expectConfigError(test_case.first, test_case.second));
    }
    return true;
  }

  bool testRejectsTooManyEntries() {
    std::string entries;
    for (int index = 0; index < 26; ++index) {
      entries += "          - {name: Output " + std::to_string(index) +
             ", index: 0x6040, sub_index: 0, bit_length: 16}\n";
    }
    const std::string yaml = replaceOnce(
      validYaml(),
      "          - {name: Control Word, index: 0x6040, sub_index: 0, bit_length: 16}\n",
      entries);
    return expectConfigError(yaml, "slaves[0].rx_pdos entries exceed MAX_PDOUTPUT_NUM");
  }

  bool testRejectsTooManyTxEntries() {
    std::string entries;
    for (int index = 0; index < 26; ++index) {
      entries += "          - {name: Input " + std::to_string(index) +
             ", index: 0x6041, sub_index: 0, bit_length: 16}\n";
    }
    const std::string yaml = replaceOnce(
      validYaml(),
      "          - {name: Status Word, index: 0x6041, sub_index: 0, bit_length: 16}\n",
      entries);
    return expectConfigError(yaml, "slaves[0].tx_pdos entries exceed MAX_PDINPUT_NUM");
  }

  bool testLoadsCheckedInExample() {
    rocos::PdoBusConfig config;
    std::string error;
    CHECK(rocos::loadPdoConfig(ROCOS_IGH_SOURCE_DIR "/config/pdo.yaml", config, error));
    CHECK(error.empty());
    CHECK(config.slaves.size() == 1U);
    CHECK(config.slaves[0].dc.has_value());
    CHECK(config.slaves[0].dc->assign_activate == 0x0300U);
    CHECK(config.slaves[0].dc->sync0_shift_ns == 0);
    CHECK(config.slaves[0].dc->sync1_cycle_ns == 0U);
    CHECK(config.slaves[0].dc->sync1_shift_ns == 0);
    CHECK(config.slaves[0].dc->reference);
    CHECK(config.slaves[0].rx_pdos[0].entries.size() == 7U);
    CHECK(config.slaves[0].tx_pdos[0].entries.size() == 5U);
    for (const std::string &name : {"talon_pdo.yaml", "ZeroErr6.yaml"}) {
        CHECK(rocos::loadPdoConfig(std::string(ROCOS_IGH_SOURCE_DIR) + "/config/" + name, config, error));
        CHECK(config.slaves.size() == 7);
        for (std::size_t i = 0; i < config.slaves.size(); ++i) {
            CHECK(config.slaves[i].dc.has_value());
            CHECK(config.slaves[i].dc->reference == (i == 0));
        }
    }
    CHECK(rocos::loadPdoConfig(ROCOS_IGH_SOURCE_DIR "/config/tc7_pdo.yaml", config, error));
    CHECK(error.empty());
    CHECK(config.slaves.size() == 8U);
    // Discovery may have populated this operational configuration on startup.
    CHECK(config.slaves[7].rx_pdos[0].index == 0x1600);
    CHECK(config.slaves[7].tx_pdos[0].index == 0x1a00);
    return true;
  }

}  // namespace

int main() {
  if (!testEmptyEntries()) return 1;
  if (!testInitCommands() || !testInvalidInitCommands()) return 1;
  if (!testMasterOptions() || !testInvalidMasterOptions()) return 1;
  if (!testLoadsValidMultiSlaveConfig()) {
    return 1;
  }
  if (!testLoadsDistributedClockConfig()) {
    return 1;
  }
  if (!testRejectsInvalidDistributedClockConfig()) {
    return 1;
  }
  if (!testRejectsInvalidConfigFields()) {
    return 1;
  }
  if (!testRejectsTooManyEntries()) {
    return 1;
  }
  if (!testRejectsTooManyTxEntries()) {
    return 1;
  }
  if (!testLoadsCheckedInExample()) {
    return 1;
  }
  return 0;
}
