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
    CHECK(config.slaves[0].rx_pdos[0].entries.size() == 3U);
    CHECK(config.slaves[0].tx_pdos[0].entries.size() == 3U);
    return true;
  }

}  // namespace

int main() {
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