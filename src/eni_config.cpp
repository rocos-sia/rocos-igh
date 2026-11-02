#include "pdo_config.hpp"
#include "shared_memory_config.hpp"

#include <libxml/parser.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_set>

namespace rocos {
namespace {
using Node = const xmlNode *;

bool named(Node node, const char *name) {
    return node && node->type == XML_ELEMENT_NODE &&
           xmlStrEqual(node->name, BAD_CAST name);
}

Node child(Node parent, const char *name, bool required = true) {
    Node result = nullptr;
    for (Node n = parent ? parent->children : nullptr; n; n = n->next) {
        if (!named(n, name)) continue;
        if (result) throw std::runtime_error(std::string(name) + " is duplicated");
        result = n;
    }
    if (!result && required) throw std::runtime_error(std::string(name) + " is required");
    return result;
}

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    return begin == std::string::npos ? "" : value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}

std::string content(Node node) {
    if (!node) return {};
    xmlChar *raw = xmlNodeGetContent(const_cast<xmlNode *>(node));
    const std::string value = raw ? reinterpret_cast<const char *>(raw) : "";
    xmlFree(raw);
    return trim(value);
}

std::string attribute(Node node, const char *name) {
    xmlChar *raw = xmlGetProp(node, BAD_CAST name);
    const std::string value = raw ? reinterpret_cast<const char *>(raw) : "";
    xmlFree(raw);
    return trim(value);
}

std::uint64_t number(std::string text, std::uint64_t maximum) {
    text = trim(std::move(text));
    const bool hex = text.rfind("#x", 0) == 0 || text.rfind("#X", 0) == 0 ||
                     text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0;
    const auto *begin = text.data() + (hex ? 2 : 0);
    const auto *end = text.data() + text.size();
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(begin, end, value, hex ? 16 : 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value > maximum)
        throw std::runtime_error("invalid or out-of-range unsigned integer: " + text);
    return value;
}

std::uint64_t value(Node node, const char *name, std::uint64_t maximum) {
    try { return number(content(child(node, name)), maximum); }
    catch (const std::exception &e) { throw std::runtime_error(std::string(name) + ": " + e.what()); }
}

std::string label(Node parent, const char *name, const std::string &fallback, std::size_t limit) {
    const auto text = content(child(parent, name, false));
    const auto result = text.empty() ? fallback : text;
    if (result.empty() || result.size() > limit)
        throw std::runtime_error(std::string(name) + " is empty or exceeds ABI name capacity");
    return result;
}

std::vector<std::uint8_t> bytes(Node command) {
    auto text = content(child(command, "Data"));
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }), text.end());
    if (text.empty() || text.size() % 2) throw std::runtime_error("Data must contain hexadecimal byte pairs");
    std::vector<std::uint8_t> result;
    for (std::size_t i = 0; i < text.size(); i += 2)
        result.push_back(static_cast<std::uint8_t>(number("#x" + text.substr(i, 2), 255)));
    return result;
}

bool transition(Node command, const char *phase) {
    for (Node n = command->children; n; n = n->next)
        if (named(n, "Transition") && content(n) == phase) return true;
    return false;
}

std::vector<PdoMappingConfig> pdos(Node process, bool rx) {
    if (!process) return {};
    const char *tag = rx ? "RxPdo" : "TxPdo";
    const unsigned int sm = rx ? 2 : 3;
    Node manager = child(process, rx ? "Sm2" : "Sm3", false);
    std::vector<std::uint16_t> assigned;
    for (Node n = manager ? manager->children : nullptr; n; n = n->next)
        if (named(n, "Pdo")) assigned.push_back(static_cast<std::uint16_t>(number(content(n), 65535)));
    if (manager) {
        if (content(child(manager, "Type")) != (rx ? "Outputs" : "Inputs"))
            throw std::runtime_error("SM2/SM3 Type does not match PDO direction");
        if (auto enable = child(manager, "Enable", false); enable && number(content(enable), 1) == 0) {
            if (!assigned.empty()) throw std::runtime_error("disabled SM has assigned PDOs");
            for (Node n = process->children; n; n = n->next)
                if (named(n, tag) && !attribute(n, "Sm").empty())
                    throw std::runtime_error("disabled SM has selected PDOs");
            return {};
        }
    }
    std::vector<Node> selected;
    std::unordered_set<std::uint16_t> declared;
    for (Node n = process->children; n; n = n->next) {
        if (!named(n, tag)) continue;
        const auto index = static_cast<std::uint16_t>(value(n, "Index", 65535));
        if (!declared.insert(index).second) throw std::runtime_error("duplicate PDO Index");
        const auto attr = attribute(n, "Sm");
        const bool listed = std::find(assigned.begin(), assigned.end(), index) != assigned.end();
        if (!attr.empty() && number(attr, 255) != sm)
            throw std::runtime_error("only RxPDO SM2 / TxPDO SM3 are supported");
        if (!assigned.empty() && !attr.empty() && !listed)
            throw std::runtime_error("PDO Sm attribute conflicts with SM Pdo list");
        if (assigned.empty() ? !attr.empty() : listed) selected.push_back(n);
    }
    // SM assignment order is authoritative, rather than the ESI catalogue order.
    if (!assigned.empty()) {
        std::vector<Node> ordered;
        std::unordered_set<std::uint16_t> seen;
        for (auto index : assigned) {
            if (!seen.insert(index).second) throw std::runtime_error("duplicate SM Pdo assignment");
            const auto found = std::find_if(selected.begin(), selected.end(), [&](Node n) {
                return value(n, "Index", 65535) == index;
            });
            if (found == selected.end()) throw std::runtime_error("SM Pdo assignment has no PDO declaration");
            ordered.push_back(*found);
        }
        selected = std::move(ordered);
    }
    std::vector<PdoMappingConfig> result;
    std::size_t count = 0;
    for (Node n : selected) {
        PdoMappingConfig pdo;
        pdo.index = static_cast<std::uint16_t>(value(n, "Index", 65535));
        const auto su = attribute(n, "Su");
        if (!su.empty() && number(su, 65535) != 0) throw std::runtime_error("only SyncUnit 0 is supported");
        for (Node e = n->children; e; e = e->next) {
            if (!named(e, "Entry")) continue;
            PdoEntryConfig entry;
            entry.index = static_cast<std::uint16_t>(value(e, "Index", 65535));
            const auto sub = child(e, "SubIndex", entry.index != 0);
            entry.sub_index = sub ? static_cast<std::uint8_t>(number(content(sub), 255)) : 0;
            entry.bit_length = static_cast<std::uint8_t>(value(e, "BitLen", 255));
            if (entry.bit_length == 0 || entry.bit_length % 8)
                throw std::runtime_error("ABI v2 requires nonzero byte-aligned PDO entries");
            entry.name = label(e, "Name", entry.index == 0 ? "Padding" :
                               "Object_" + std::to_string(entry.index) + "_" + std::to_string(entry.sub_index),
                               MAX_PD_NAME_LEN - 1);
            if (++count > (rx ? MAX_PDOUTPUT_NUM : MAX_PDINPUT_NUM))
                throw std::runtime_error("PDO entries exceed ABI capacity");
            pdo.entries.push_back(std::move(entry));
        }
        result.push_back(std::move(pdo));
    }
    return result;
}

void initCommands(Node slave, SlavePdoConfig &config) {
    Node coe = child(child(slave, "Mailbox", false), "CoE", false);
    Node commands = child(coe, "InitCmds", false);
    for (Node c = commands ? commands->children : nullptr; c; c = c->next) {
        if (!named(c, "InitCmd")) continue;
        if (content(child(c, "Transition")) != "PS" || value(c, "Ccs", 255) != 1)
            throw std::runtime_error("only PS CoE download InitCmds are supported");
        InitCommandConfig command;
        command.index = static_cast<std::uint16_t>(value(c, "Index", 65535));
        // PDO assignment/mapping is owned by the existing IgH configuration path.
        if ((command.index >= 0x1600 && command.index <= 0x1bff) ||
            command.index == 0x1c12 || command.index == 0x1c13) continue;
        const auto ca = attribute(c, "CompleteAccess");
        if (!ca.empty() && number(ca, 1) != 0)
            throw std::runtime_error("CompleteAccess application InitCmd is unsupported");
        command.sub_index = static_cast<std::uint8_t>(value(c, "SubIndex", 255));
        const auto data = bytes(c);
        if (data.size() != 1 && data.size() != 2 && data.size() != 4 && data.size() != 8)
            throw std::runtime_error("application InitCmd Data must contain 1, 2, 4 or 8 bytes");
        command.size = static_cast<std::uint8_t>(data.size());
        std::copy(data.begin(), data.end(), command.data.begin());
        config.init_commands.push_back(command);
    }
}

void clocks(Node slave, SlavePdoConfig &config) {
    Node dc = child(slave, "DC", false);
    if (!dc) return;
    DistributedClockConfig clock;
    clock.sync0_cycle_ns = static_cast<std::uint32_t>(value(dc, "CycleTime0", UINT32_MAX));
    clock.sync1_cycle_ns = static_cast<std::uint32_t>(value(dc, "CycleTime1", UINT32_MAX));
    clock.reference = value(dc, "ReferenceClock", 1) != 0;
    const auto shift = content(child(dc, "ShiftTime"));
    const auto parsed = std::from_chars(shift.data(), shift.data() + shift.size(), clock.sync0_shift_ns);
    if (parsed.ec != std::errc{} || parsed.ptr != shift.data() + shift.size())
        throw std::runtime_error("invalid DC ShiftTime");
    if (clock.sync0_cycle_ns == 0) {
        if (clock.reference || clock.sync1_cycle_ns) throw std::runtime_error("inactive DC has reference or SYNC1");
        return;
    }
    Node commands = child(slave, "InitCmds", false);
    std::optional<std::uint16_t> activation;
    for (Node c = commands ? commands->children : nullptr; c; c = c->next) {
        if (!named(c, "InitCmd") || !transition(c, "PS")) continue;
        const auto ado = value(c, "Ado", 65535);
        if (ado != 0x980 && ado != 0x981) continue;
        const auto cmd = value(c, "Cmd", 255);
        if (cmd != 2 && cmd != 5) throw std::runtime_error("unsupported DC activation register command");
        const auto data = bytes(c);
        if (data.size() != (ado == 0x980 ? 2U : 1U)) throw std::runtime_error("invalid DC activation Data");
        const auto assign = static_cast<std::uint16_t>(ado == 0x980 ? data[0] | (data[1] << 8) : data[0] << 8);
        if (activation && *activation != assign) throw std::runtime_error("conflicting DC activation commands");
        activation = assign;
    }
    if (!activation || !(*activation & 0x100))
        throw std::runtime_error("DC requires a PS activation write at 0x0980/0x0981 enabling SYNC0");
    clock.assign_activate = *activation;
    config.dc = clock;
}
} // namespace

bool loadEniConfig(const std::string &path, PdoBusConfig &config, std::string &error) noexcept {
    try {
        error.clear();
        // Do not load DTDs, expand entities, fetch a schema or permit network I/O.
        std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
            xmlReadFile(path.c_str(), nullptr, XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                        XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc);
        if (!doc) throw std::runtime_error("cannot read or parse ENI XML");
        if (doc->intSubset || doc->extSubset) throw std::runtime_error("ENI DTDs are unsupported");
        Node root = xmlDocGetRootElement(doc.get());
        if (!named(root, "EtherCATConfig")) throw std::runtime_error("expected EtherCATConfig root");
        Node bus = child(root, "Config");
        PdoBusConfig loaded;
        loaded.from_eni = true;
        Node cyclic = child(bus, "Cyclic", false);
        if (cyclic) loaded.master.period_us = static_cast<std::uint32_t>(value(cyclic, "CycleTime", UINT32_MAX));
        if (loaded.master.period_us < 1000) throw std::runtime_error("Cyclic/CycleTime must be >= 1000 us");
        std::unordered_set<std::uint16_t> stations;
        std::size_t references = 0;
        for (Node s = bus->children; s; s = s->next) {
            if (!named(s, "Slave")) continue;
            if (loaded.slaves.size() == MAX_SLAVE_NUM) throw std::runtime_error("slaves exceed MAX_SLAVE_NUM");
            SlavePdoConfig slave;
            Node info = child(s, "Info");
            slave.id = static_cast<std::uint16_t>(0U - value(info, "AutoIncAddr", 65535));
            const auto station = static_cast<std::uint16_t>(value(info, "PhysAddr", 65535));
            if (station == 0 || !stations.insert(station).second) throw std::runtime_error("invalid or duplicate PhysAddr");
            slave.name = label(info, "Name", "", MAX_SLAVE_NAME_LEN - 1);
            slave.vendor_id = static_cast<std::uint32_t>(value(info, "VendorId", UINT32_MAX));
            slave.product_code = static_cast<std::uint32_t>(value(info, "ProductCode", UINT32_MAX));
            if (!slave.vendor_id || !slave.product_code) throw std::runtime_error("ENI identity must be nonzero");
            try {
                Node process = child(s, "ProcessData", false);
                for (Node n = process ? process->children : nullptr; n; n = n->next) {
                    if (n->type != XML_ELEMENT_NODE) continue;
                    const std::string tag(reinterpret_cast<const char *>(n->name));
                    if (tag.rfind("Sm", 0) != 0 || tag == "Sm2" || tag == "Sm3") continue;
                    const auto type = content(child(n, "Type", false));
                    if (type == "Inputs" || type == "Outputs")
                        throw std::runtime_error("only process data SM2/SM3 are supported");
                }
                slave.rx_pdos = pdos(process, true);
                slave.tx_pdos = pdos(process, false);
                clocks(s, slave);
                initCommands(s, slave);
            } catch (const std::exception &e) {
                throw std::runtime_error("slave[position=" + std::to_string(slave.id) + "]: " + e.what());
            }
            if (slave.dc) {
                loaded.master.dc_enabled = true;
                if (slave.dc->reference) ++references;
            }
            loaded.slaves.push_back(std::move(slave));
        }
        if (loaded.slaves.empty()) throw std::runtime_error("ENI has no slaves");
        std::sort(loaded.slaves.begin(), loaded.slaves.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
        for (std::size_t i = 0; i < loaded.slaves.size(); ++i)
            if (loaded.slaves[i].id != i) throw std::runtime_error("AutoIncAddr positions must be unique and contiguous from 0");
        if (loaded.master.dc_enabled && references != 1) throw std::runtime_error("ENI DC requires exactly one reference clock");
        config = std::move(loaded);
        return true;
    } catch (const std::exception &e) {
        config = {};
        error = path + ": " + e.what();
        return false;
    }
}
} // namespace rocos
