#include "pdo_discovery.hpp"
#include "shared_memory_config.hpp"

#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace rocos {
namespace {
std::string address(std::uint16_t position, std::uint16_t index, unsigned int sub) {
    std::ostringstream out;
    out << "PDO discovery slave[position=" << position << "] object=0x"
        << std::hex << std::setfill('0') << std::setw(4) << index << ':' << std::setw(2) << sub;
    return out.str();
}
} // namespace

bool discoverEmptyPdoEntries(ec_master_t *master, PdoBusConfig &config,
                              const std::string &path,
                              std::chrono::steady_clock::time_point deadline,
                              std::string &error) noexcept {
    try {
        error.clear();
        PdoBusConfig resolved = config;
        bool discovered = false;
        const auto upload = [&](std::uint16_t position, std::uint16_t index,
                                unsigned int sub, std::size_t expected_size) {
            const auto context = address(position, index, sub);
            const auto guard = [&] {
                if (master == nullptr) throw std::runtime_error(context + ": master is null");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error(context + ": PREOP timeout");
                ec_slave_info_t info{};
                const int rc = ecrt_master_get_slave(master, position, &info);
                if (rc != 0 || info.al_state != EC_AL_STATE_PREOP || info.error_flag != 0)
                    throw std::runtime_error(context + ": slave is not in healthy PREOP (rc=" +
                        std::to_string(rc) + ", state=" + std::to_string(info.al_state) + ")");
            };
            guard();
            std::array<std::uint8_t, 4> bytes{};
            std::size_t size = 0;
            std::uint32_t abort_code = 0;
            const int rc = ecrt_master_sdo_upload(master, position, index,
                static_cast<std::uint8_t>(sub), bytes.data(), bytes.size(), &size, &abort_code);
            guard(); // Blocking upload time counts toward the same PREOP budget.
            if (rc != 0) {
                std::ostringstream out;
                out << context << ": SDO read failed rc=" << rc << " abort=0x" << std::hex << abort_code;
                throw std::runtime_error(out.str());
            }
            if (size != expected_size)
                throw std::runtime_error(context + ": unexpected SDO size=" + std::to_string(size) +
                    ", expected=" + std::to_string(expected_size));
            std::uint32_t value = 0;
            for (std::size_t i = 0; i < size; ++i) value |= std::uint32_t{bytes[i]} << (8 * i);
            return value;
        };
        for (auto &slave : resolved.slaves) {
            for (const bool rx : {true, false}) {
                auto &pdos = rx ? slave.rx_pdos : slave.tx_pdos;
                const std::size_t limit = rx ? MAX_PDOUTPUT_NUM : MAX_PDINPUT_NUM;
                std::size_t total = 0;
                for (const auto &pdo : pdos) total += pdo.entries.size();
                if (total > limit) throw std::runtime_error("configured PDO entries exceed ABI limit");
                for (auto &pdo : pdos) {
                    if (!pdo.entries.empty()) continue;
                    const auto count = upload(slave.id, pdo.index, 0, 1);
                    if (count > limit - total)
                        throw std::runtime_error(address(slave.id, pdo.index, 0) +
                            (rx ? ": entries exceed MAX_PDOUTPUT_NUM" : ": entries exceed MAX_PDINPUT_NUM"));
                    total += count;
                    for (unsigned int sub = 1; sub <= count; ++sub) {
                        const auto value = upload(slave.id, pdo.index, sub, 4);
                        PdoEntryConfig entry;
                        entry.index = static_cast<std::uint16_t>(value >> 16);
                        entry.sub_index = static_cast<std::uint8_t>(value >> 8);
                        entry.bit_length = static_cast<std::uint8_t>(value);
                        if (entry.bit_length == 0) {
                            std::ostringstream message;
                            message << address(slave.id, pdo.index, sub)
                                    << ": bit_length must be between 1 and 255"
                                    << "; mapping=0x" << std::hex << std::setfill('0') << std::setw(8) << value
                                    << " decoded{index=0x" << std::setw(4) << entry.index
                                    << ", sub_index=0x" << std::setw(2) << unsigned(entry.sub_index)
                                    << ", bit_length=" << std::dec << unsigned(entry.bit_length) << "}"
                                    << " bytes(le)=" << std::hex;
                            for (unsigned int byte = 0; byte < 4; ++byte)
                                message << (byte == 0 ? "" : " ") << std::setw(2) << ((value >> (8 * byte)) & 0xffU);
                            throw std::runtime_error(message.str());
                        }
                        std::ostringstream name;
                        name << (rx ? "RxPDO_" : "TxPDO_") << std::hex << std::uppercase
                             << std::setfill('0') << std::setw(4) << pdo.index << '_'
                             << std::dec << std::setw(2) << sub;
                        entry.name = name.str();
                        pdo.entries.push_back(std::move(entry));
                    }
                    // Detect a mapping count changing during the read.
                    if (upload(slave.id, pdo.index, 0, 1) != count)
                        throw std::runtime_error(address(slave.id, pdo.index, 0) + ": mapping changed during discovery");
                    discovered = true;
                }
            }
        }
        if (!discovered) return true;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("PDO discovery PREOP timeout before YAML writeback");
        if (!saveDiscoveredPdoConfig(path, config, resolved, error)) return false;
        config = std::move(resolved);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
} // namespace rocos
