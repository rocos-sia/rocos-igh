#include "pdo_config.hpp"

#include "shared_memory_config.hpp"

#include <yaml-cpp/yaml.h>

#include <charconv>
#include <initializer_list>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <type_traits>

namespace rocos {
namespace {

const YAML::Node requireNode(const YAML::Node &parent,
                             const char *key,
                             const std::string &path) {
    const YAML::Node node = parent[key];
    if (!node) {
        throw std::runtime_error(path + "." + key + " is required");
    }
    return node;
}

void rejectUnknownKeys(const YAML::Node &node,
                       std::initializer_list<const char *> allowed,
                       const std::string &path) {
    std::unordered_set<std::string> seen;
    for (const auto &field : node) {
        const std::string key = field.first.as<std::string>();
        if (!seen.insert(key).second) {
            throw std::runtime_error(path + "." + key + " is duplicated");
        }
        bool known = false;
        for (const char *allowed_key : allowed) {
            if (key == allowed_key) {
                known = true;
                break;
            }
        }
        if (!known) {
            throw std::runtime_error(path + "." + key + " is not allowed");
        }
    }
}

std::string parseString(const YAML::Node &node,
                        const std::string &path,
                        std::size_t maximum_length) {
    if (!node.IsScalar()) {
        throw std::runtime_error(path + " must be a string");
    }
    const std::string value = node.as<std::string>();
    if (value.empty()) {
        throw std::runtime_error(path + " must be non-empty");
    }
    if (value.size() > maximum_length) {
        throw std::runtime_error(path + " is too long");
    }
    return value;
}

std::uint64_t parseUnsigned(const YAML::Node &node,
                            const std::string &path,
                            std::uint64_t maximum) {
    if (!node.IsScalar() || node.Tag() != "?") {
        throw std::runtime_error(path + " must be an unsigned integer");
    }

    const std::string text = node.Scalar();
    const bool hexadecimal = text.size() > 2U && text[0] == '0' &&
                             (text[1] == 'x' || text[1] == 'X');
    const std::size_t first_digit = hexadecimal ? 2U : 0U;
    if (first_digit == text.size()) {
        throw std::runtime_error(path + " must be an unsigned integer");
    }
    for (std::size_t index = first_digit; index < text.size(); ++index) {
        const char character = text[index];
        const bool decimal_digit = character >= '0' && character <= '9';
        const bool hexadecimal_digit = decimal_digit ||
                                       (character >= 'a' && character <= 'f') ||
                                       (character >= 'A' && character <= 'F');
        if ((hexadecimal && !hexadecimal_digit) || (!hexadecimal && !decimal_digit)) {
            throw std::runtime_error(path + " must be an unsigned integer");
        }
    }

    std::size_t parsed = 0;
    std::uint64_t value = 0;
    try {
        const int base = hexadecimal ? 16 : 10;
        value = std::stoull(text, &parsed, base);
    } catch (const std::exception &) {
        throw std::runtime_error(path + " must be an unsigned integer");
    }
    if (parsed != text.size() || value > maximum) {
        throw std::runtime_error(path + " is out of range");
    }
    return value;
}

std::int64_t parseSigned(const YAML::Node &node,
                         const std::string &path,
                         std::int64_t minimum,
                         std::int64_t maximum) {
    if (!node.IsScalar() || node.Tag() != "?") {
        throw std::runtime_error(path + " must be a signed integer");
    }

    const std::string text = node.Scalar();
    if (text.empty() || text.front() == '+') {
        throw std::runtime_error(path + " must be a signed integer");
    }

    std::int64_t value = 0;
    const char *const begin = text.data();
    const char *const end = begin + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::runtime_error(path + " must be a signed integer");
    }
    if (value < minimum || value > maximum) {
        throw std::runtime_error(path + " is out of range");
    }
    return value;
}

std::vector<InitCommandConfig> parseInitCommands(const YAML::Node &node,
                                                const std::string &path) {
    if (!node.IsSequence()) {
        throw std::runtime_error(path + " must be a sequence");
    }
    std::vector<InitCommandConfig> commands;
    commands.reserve(node.size());
    for (std::size_t i = 0; i < node.size(); ++i) {
        const auto entry = node[i];
        const auto entry_path = path + "[" + std::to_string(i) + "]";
        if (!entry.IsMap()) throw std::runtime_error(entry_path + " must be a mapping");
        rejectUnknownKeys(entry, {"name", "index", "sub_index", "type", "value", "Type", "Value"},
                          entry_path);
        // Accept Type/Value as requested, alongside the existing lowercase style.
        const auto field = [&](const char *lower, const char *upper) {
            if (entry[lower] && entry[upper]) {
                throw std::runtime_error(entry_path + "." + lower + " is duplicated by " + upper);
            }
            return entry[upper] ? entry[upper] : requireNode(entry, lower, entry_path);
        };
        if (entry["name"]) parseString(entry["name"], entry_path + ".name", MAX_PD_NAME_LEN - 1U);
        InitCommandConfig command;
        command.index = static_cast<std::uint16_t>(parseUnsigned(
            requireNode(entry, "index", entry_path), entry_path + ".index", 0xffff));
        command.sub_index = static_cast<std::uint8_t>(parseUnsigned(
            requireNode(entry, "sub_index", entry_path), entry_path + ".sub_index", 0xff));
        const auto type = parseString(field("type", "Type"), entry_path + ".type", 16);
        bool is_signed = false;
        unsigned int bits = 0;
        for (unsigned int width : {8U, 16U, 32U, 64U}) {
            if (type == "int" + std::to_string(width)) { bits = width; is_signed = true; }
            if (type == "uint" + std::to_string(width)) bits = width;
        }
        if (bits == 0) {
            throw std::runtime_error(entry_path + ".type must be int8/uint8/int16/uint16/int32/uint32/int64/uint64");
        }
        const auto value = field("value", "Value");
        const auto value_path = entry_path + ".value";
        std::uint64_t encoded = 0;
        if (is_signed) {
            const auto minimum = bits == 64 ? std::numeric_limits<std::int64_t>::min()
                                           : -(std::int64_t{1} << (bits - 1));
            const auto maximum = bits == 64 ? std::numeric_limits<std::int64_t>::max()
                                           : (std::int64_t{1} << (bits - 1)) - 1;
            // Positive hexadecimal values are accepted only within the signed range.
            if (value.IsScalar() && value.Scalar().rfind("0x", 0) == 0) {
                encoded = parseUnsigned(value, value_path, static_cast<std::uint64_t>(maximum));
            } else if (value.IsScalar() && value.Scalar().rfind("0X", 0) == 0) {
                encoded = parseUnsigned(value, value_path, static_cast<std::uint64_t>(maximum));
            } else {
                encoded = static_cast<std::uint64_t>(parseSigned(value, value_path, minimum, maximum));
            }
        } else {
            const auto maximum = bits == 64 ? std::numeric_limits<std::uint64_t>::max()
                                           : (std::uint64_t{1} << bits) - 1;
            encoded = parseUnsigned(value, value_path, maximum);
        }
        command.size = static_cast<std::uint8_t>(bits / 8);
        for (std::size_t b = 0; b < command.size; ++b)
            command.data[b] = static_cast<std::uint8_t>(encoded >> (b * 8));
        commands.push_back(command);
    }
    return commands;
}

RuntimeOptions parseMasterOptions(const YAML::Node &node, RuntimeOptions options = {},
                                  bool flat = false) {
    if (!node.IsMap()) {
        throw std::runtime_error("master must be a mapping");
    }
    if (!flat) {
        rejectUnknownKeys(node, {"master_id", "period_us", "cpu", "dc",
                                "preop_timeout_ms", "op_timeout_ms", "pdo_config_attempts"},
                          "master");
    }
    const std::string prefix = flat ? "root." : "master.";
    const auto integer = [&](const char *key, auto &value, std::uint64_t minimum) {
        if (!node[key]) return;
        using T = std::decay_t<decltype(value)>;
        const std::string path = prefix + key;
        const auto parsed = parseUnsigned(node[key], path, std::numeric_limits<T>::max());
        if (parsed < minimum) {
            throw std::runtime_error(path + " must be >= " + std::to_string(minimum));
        }
        value = static_cast<T>(parsed);
    };
    integer(flat ? "id" : "master_id", options.master_id, 0);
    integer(flat ? "cycle" : "period_us", options.period_us, 1000);
    integer("preop_timeout_ms", options.preop_timeout_ms, 1);
    integer("op_timeout_ms", options.op_timeout_ms, 1);
    integer("pdo_config_attempts", options.pdo_config_attempts, 1);
    const char *cpu_key = flat ? "cpuidx" : "cpu";
    if (node[cpu_key]) {
        options.cpu.reset();
        if (!node[cpu_key].IsNull()) {
            unsigned int cpu = 0;
            integer(cpu_key, cpu, 0);
            options.cpu = cpu;
        }
    }
    if (node["dc"]) {
        const auto dc = parseString(node["dc"], prefix + "dc", 3);
        if (dc != "on" && dc != "off") {
            throw std::runtime_error(prefix + "dc must be on or off");
        }
        options.dc_enabled = dc == "on";
    }
    // Cross-field DC/period validation runs after the CLI overrides are merged.
    return options;
}

RuntimeOptions parseStartupOptions(const YAML::Node &root, RuntimeOptions defaults = {}) {
    // Keep existing PDO YAML files working, but never accept ambiguous mixed layouts.
    if (root["master"]) {
        for (const char *key : {"id", "cycle", "cpuidx", "dc", "preop_timeout_ms",
                                "op_timeout_ms", "pdo_config_attempts"}) {
            if (root[key]) throw std::runtime_error(std::string("root.") + key +
                " cannot be combined with the legacy master block");
        }
        return parseMasterOptions(root["master"], defaults);
    }
    return parseMasterOptions(root, defaults, true);
}

std::vector<PdoMappingConfig> parsePdos(const YAML::Node &node,
                                        const std::string &path) {
    if (!node.IsSequence() || node.size() == 0U) {
        throw std::runtime_error(path + " must be a non-empty sequence");
    }

    std::vector<PdoMappingConfig> pdos;
    pdos.reserve(node.size());
    for (std::size_t pdo_index = 0; pdo_index < node.size(); ++pdo_index) {
        const YAML::Node pdo_node = node[pdo_index];
        const std::string pdo_path = path + "[" + std::to_string(pdo_index) + "]";
        if (!pdo_node.IsMap()) {
            throw std::runtime_error(pdo_path + " must be a mapping");
        }
        rejectUnknownKeys(pdo_node, {"index", "entries"}, pdo_path);

        PdoMappingConfig pdo;
        pdo.index = static_cast<std::uint16_t>(parseUnsigned(
            requireNode(pdo_node, "index", pdo_path), pdo_path + ".index",
            std::numeric_limits<std::uint16_t>::max()));

        const YAML::Node entries = requireNode(pdo_node, "entries", pdo_path);
        // Accept explicit empty YAML values while keeping populated entries a sequence.
        if (!entries.IsSequence() && !entries.IsNull() &&
            !(entries.IsMap() && entries.size() == 0U)) {
            throw std::runtime_error(pdo_path + ".entries must be a sequence or empty (null / {})");
        }
        pdo.entries.reserve(entries.size());
        for (std::size_t entry_index = 0; entry_index < entries.size(); ++entry_index) {
            const YAML::Node entry_node = entries[entry_index];
            const std::string entry_path = pdo_path + ".entries[" +
                                           std::to_string(entry_index) + "]";
            if (!entry_node.IsMap()) {
                throw std::runtime_error(entry_path + " must be a mapping");
            }
            rejectUnknownKeys(entry_node,
                              {"name", "index", "sub_index", "bit_length"},
                              entry_path);

            PdoEntryConfig entry;
            entry.name = parseString(requireNode(entry_node, "name", entry_path),
                                     entry_path + ".name", MAX_PD_NAME_LEN - 1U);
            entry.index = static_cast<std::uint16_t>(parseUnsigned(
                requireNode(entry_node, "index", entry_path), entry_path + ".index",
                std::numeric_limits<std::uint16_t>::max()));
            entry.sub_index = static_cast<std::uint8_t>(parseUnsigned(
                requireNode(entry_node, "sub_index", entry_path),
                entry_path + ".sub_index", std::numeric_limits<std::uint8_t>::max()));
            entry.bit_length = static_cast<std::uint8_t>(parseUnsigned(
                requireNode(entry_node, "bit_length", entry_path),
                entry_path + ".bit_length", std::numeric_limits<std::uint8_t>::max()));
            if (entry.bit_length % 8U != 0U) {
                throw std::runtime_error(entry_path + ".bit_length: ABI v2 requires byte-aligned PDO entries");
            }
            if (entry.bit_length == 0U) {
                throw std::runtime_error(entry_path +
                                         ".bit_length must be between 1 and 255");
            }
            pdo.entries.push_back(std::move(entry));
        }
        pdos.push_back(std::move(pdo));
    }
    return pdos;
}

}  // namespace

bool loadPdoConfig(const std::string &path,
                   PdoBusConfig &config,
                   std::string &error) noexcept {
    try {
        error.clear();
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open configuration file");
        const std::string source((std::istreambuf_iterator<char>(input)), {});
        if (input.bad()) throw std::runtime_error("cannot read configuration file");
        // --config always names a YAML startup file; ENI is loaded only via eni:.
        const auto start = source.find_first_not_of(" \t\r\n", source.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0);
        if (start != std::string::npos && source[start] == '<')
            throw std::runtime_error("--config requires YAML; reference the ENI with eni: <path> in igh.yaml");
        const YAML::Node root = YAML::Load(source);
        if (!root.IsMap()) {
            throw std::runtime_error("root must be a mapping");
        }
        rejectUnknownKeys(root, {"version", "master", "slaves", "eni", "id", "cycle", "cpuidx",
                                "dc", "preop_timeout_ms", "op_timeout_ms", "pdo_config_attempts"}, "root");

        // The ENI startup format mirrors ECM and does not require a version field.
        // Preserve the existing version requirement for inline PDO descriptions.
        if (root["version"] || !root["eni"]) {
            const std::uint64_t version = parseUnsigned(
                requireNode(root, "version", "root"), "version",
                std::numeric_limits<std::uint64_t>::max());
            if (version != 1U) throw std::runtime_error("version must be 1");
        }

        if (root["eni"]) {
            if (root["slaves"]) throw std::runtime_error("eni and slaves are mutually exclusive");
            const auto eni = parseString(root["eni"], "eni", 4096);
            PdoBusConfig loaded;
            const auto target = std::filesystem::path(path).parent_path() / eni;
            if (!loadEniConfig(target.string(), loaded, error)) {
                error = path + ": " + error;
                config = {};
                return false;
            }
            loaded.master = parseStartupOptions(root, loaded.master);
            config = std::move(loaded);
            return true;
        }

        const YAML::Node slaves = requireNode(root, "slaves", "root");
        if (!slaves.IsSequence() || slaves.size() == 0U) {
            throw std::runtime_error("slaves must be a non-empty sequence");
        }
        if (slaves.size() > MAX_SLAVE_NUM) {
            throw std::runtime_error("slaves exceed MAX_SLAVE_NUM");
        }

        PdoBusConfig loaded;
        loaded.source_yaml = source;
        loaded.master = parseStartupOptions(root);
        loaded.slaves.reserve(slaves.size());
        for (std::size_t slave_index = 0; slave_index < slaves.size(); ++slave_index) {
            const YAML::Node slave_node = slaves[slave_index];
            const std::string slave_path = "slaves[" + std::to_string(slave_index) + "]";
            if (!slave_node.IsMap()) {
                throw std::runtime_error(slave_path + " must be a mapping");
            }
            rejectUnknownKeys(slave_node, {"id", "name", "dc", "rx_pdos", "tx_pdos", "init_commands"},
                              slave_path);

            SlavePdoConfig slave;
            slave.id = static_cast<std::uint16_t>(parseUnsigned(
                requireNode(slave_node, "id", slave_path), slave_path + ".id",
                std::numeric_limits<std::uint16_t>::max()));
            if (slave.id != slave_index) {
                throw std::runtime_error(slave_path + ".id must match its zero-based sequence index");
            }
            slave.name = parseString(requireNode(slave_node, "name", slave_path),
                                     slave_path + ".name", MAX_SLAVE_NAME_LEN - 1U);
            const auto dc_node = slave_node["dc"];
            const std::string dc = dc_node && dc_node.IsScalar() ? dc_node.Scalar() : "on";
            if ((dc_node && !dc_node.IsScalar()) || (dc != "ref" && dc != "on" && dc != "off")) {
                throw std::runtime_error(slave_path + ".dc must be ref, on or off");
            }
            // Keep the selection even when master DC is off so CLI can enable it.
            if (dc != "off") {
                slave.dc = DistributedClockConfig{0x0300U, 0, 0U, 0, dc == "ref"};
            }
            if (slave_node["init_commands"]) {
                slave.init_commands = parseInitCommands(slave_node["init_commands"],
                                                       slave_path + ".init_commands");
            }
            slave.rx_pdos = parsePdos(requireNode(slave_node, "rx_pdos", slave_path),
                                      slave_path + ".rx_pdos");
            slave.tx_pdos = parsePdos(requireNode(slave_node, "tx_pdos", slave_path),
                                      slave_path + ".tx_pdos");

            std::size_t output_entries = 0;
            for (const PdoMappingConfig &pdo : slave.rx_pdos) {
                output_entries += pdo.entries.size();
            }
            if (output_entries > MAX_PDOUTPUT_NUM) {
                throw std::runtime_error(slave_path +
                                         ".rx_pdos entries exceed MAX_PDOUTPUT_NUM");
            }

            std::size_t input_entries = 0;
            for (const PdoMappingConfig &pdo : slave.tx_pdos) {
                input_entries += pdo.entries.size();
            }
            if (input_entries > MAX_PDINPUT_NUM) {
                throw std::runtime_error(slave_path +
                                         ".tx_pdos entries exceed MAX_PDINPUT_NUM");
            }
            loaded.slaves.push_back(std::move(slave));
        }

        std::size_t reference_count = 0;
        for (const SlavePdoConfig &slave : loaded.slaves) {
            if (slave.dc.has_value() && slave.dc->reference) {
                ++reference_count;
            }
        }
        if (reference_count > 1U) {
            throw std::runtime_error("only one DC reference slave is allowed");
        }

        config = std::move(loaded);
        return true;
    } catch (const std::exception &exception) {
        config = PdoBusConfig{};
        error = path + ": " + exception.what();
        return false;
    }
}

}  // namespace rocos
