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
    return replaceOnce(validYaml(), "    rx_pdos:\n", R"yaml(    dc:
      assign_activate: 0x0300
      sync0_shift_ns: -250000
      sync1_cycle_ns: 2000000
      sync1_shift_ns: 125000
      reference: true
    rx_pdos:
)yaml");
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
  const TemporaryYamlFile file(validDcYaml());
  rocos::PdoBusConfig config;
  std::string error;
  CHECK(rocos::loadPdoConfig(file.path(), config, error));
  CHECK(error.empty());
  CHECK(config.slaves.size() == 1U);
  CHECK(config.slaves[0].dc.has_value());
  CHECK(config.slaves[0].dc->assign_activate == 0x0300U);
  CHECK(config.slaves[0].dc->sync0_shift_ns == -250000);
  CHECK(config.slaves[0].dc->sync1_cycle_ns == 2000000U);
  CHECK(config.slaves[0].dc->sync1_shift_ns == 125000);
  CHECK(config.slaves[0].dc->reference);

  const TemporaryYamlFile defaults_file(replaceOnce(
    validDcYaml(),
    "      sync0_shift_ns: -250000\n      sync1_cycle_ns: 2000000\n"
    "      sync1_shift_ns: 125000\n      reference: true\n",
    ""));
  CHECK(rocos::loadPdoConfig(defaults_file.path(), config, error));
  CHECK(config.slaves[0].dc.has_value());
  CHECK(config.slaves[0].dc->sync0_shift_ns == 0);
  CHECK(config.slaves[0].dc->sync1_cycle_ns == 0U);
  CHECK(config.slaves[0].dc->sync1_shift_ns == 0);
  CHECK(!config.slaves[0].dc->reference);
  return true;
}

bool testRejectsInvalidDistributedClockConfig() {
  std::vector<std::pair<std::string, std::string>> cases;
  cases.emplace_back(replaceOnce(validDcYaml(), "      assign_activate: 0x0300\n", ""),
             "dc.assign_activate is required");
  cases.emplace_back(replaceOnce(validDcYaml(), "assign_activate: 0x0300", "assign_activate: 0"),
             "dc.assign_activate must be non-zero");
  cases.emplace_back(replaceOnce(validDcYaml(), "assign_activate: 0x0300", "assign_activate: 65536"),
             "dc.assign_activate is out of range");
  cases.emplace_back(replaceOnce(validDcYaml(), "sync0_shift_ns: -250000", "sync0_shift_ns: 2147483648"),
             "dc.sync0_shift_ns is out of range");
  cases.emplace_back(replaceOnce(validDcYaml(), "sync1_cycle_ns: 2000000", "sync1_cycle_ns: 4294967296"),
             "dc.sync1_cycle_ns is out of range");
  cases.emplace_back(replaceOnce(validDcYaml(), "reference: true", "reference: yes"),
             "dc.reference must be a boolean");
  cases.emplace_back(replaceOnce(validDcYaml(), "reference: true", "reference: \"true\""),
             "dc.reference must be a boolean");
  cases.emplace_back(replaceOnce(validDcYaml(), "      reference: true\n", "      reference: true\n      typo: 1\n"),
             "dc.typo is not allowed");

  for (const auto &test_case : cases) {
    CHECK(expectConfigError(test_case.first, test_case.second));
  }

  const TemporaryYamlFile duplicate_reference_file(R"yaml(version: 1
slaves:
  - id: 0
    name: First
    dc:
      assign_activate: 0x0300
      reference: true
    rx_pdos:
      - index: 0x1600
        entries:
          - {name: Control, index: 0x6040, sub_index: 0, bit_length: 16}
    tx_pdos:
      - index: 0x1A00
        entries:
          - {name: Status, index: 0x6041, sub_index: 0, bit_length: 16}
  - id: 1
    name: Second
    dc:
      assign_activate: 0x0300
      reference: true
    rx_pdos:
      - index: 0x1600
        entries:
          - {name: Control, index: 0x6040, sub_index: 0, bit_length: 16}
    tx_pdos:
      - index: 0x1A00
        entries:
          - {name: Status, index: 0x6041, sub_index: 0, bit_length: 16}
)yaml");
  rocos::PdoBusConfig config;
  std::string error;
  CHECK(!rocos::loadPdoConfig(duplicate_reference_file.path(), config, error));
  CHECK(error.find("only one DC reference") != std::string::npos);
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
    cases.emplace_back(replaceOnce(validYaml(), "bit_length: 16", "bit_length: 7"),
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
      cases.emplace_back(replaceOnce(
                   validYaml(),
                   "entries:\n          - {name: Control Word, index: 0x6040, sub_index: 0, bit_length: 16}",
                   "entries: []"),
                 "slaves[0].rx_pdos[0].entries");
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
    return true;
  }

}  // namespace

int main() {
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
