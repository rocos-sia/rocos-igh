#include "pdo_config.hpp"

#include "shared_memory_config.hpp"

#include <yaml-cpp/yaml.h>

#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

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
        if (!entries.IsSequence() || entries.size() == 0U) {
            throw std::runtime_error(pdo_path + ".entries must be a non-empty sequence");
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
            if (entry.bit_length == 0U || (entry.bit_length % 8U) != 0U) {
                throw std::runtime_error(entry_path +
                                         ".bit_length must be a non-zero multiple of 8");
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
        const YAML::Node root = YAML::LoadFile(path);
        if (!root.IsMap()) {
            throw std::runtime_error("root must be a mapping");
        }
        rejectUnknownKeys(root, {"version", "slaves"}, "root");

        const std::uint64_t version = parseUnsigned(
            requireNode(root, "version", "root"), "version",
            std::numeric_limits<std::uint64_t>::max());
        if (version != 1U) {
            throw std::runtime_error("version must be 1");
        }

        const YAML::Node slaves = requireNode(root, "slaves", "root");
        if (!slaves.IsSequence() || slaves.size() == 0U) {
            throw std::runtime_error("slaves must be a non-empty sequence");
        }
        if (slaves.size() > MAX_SLAVE_NUM) {
            throw std::runtime_error("slaves exceed MAX_SLAVE_NUM");
        }

        PdoBusConfig loaded;
        loaded.slaves.reserve(slaves.size());
        for (std::size_t slave_index = 0; slave_index < slaves.size(); ++slave_index) {
            const YAML::Node slave_node = slaves[slave_index];
            const std::string slave_path = "slaves[" + std::to_string(slave_index) + "]";
            if (!slave_node.IsMap()) {
                throw std::runtime_error(slave_path + " must be a mapping");
            }
            rejectUnknownKeys(slave_node, {"id", "name", "rx_pdos", "tx_pdos"},
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

        config = std::move(loaded);
        return true;
    } catch (const std::exception &exception) {
        config = PdoBusConfig{};
        error = path + ": " + exception.what();
        return false;
    }
}

}  // namespace rocos