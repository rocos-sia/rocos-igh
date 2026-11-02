#include "startup_report.hpp"
#include "console_style.hpp"

#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>

namespace rocos {
namespace {
std::string absolutePath(const std::string &path) {
    std::error_code error;
    const auto resolved = std::filesystem::absolute(path, error);
    return error ? path : resolved.lexically_normal().string();
}

std::string cpuName(const RuntimeOptions &options) {
    return options.cpu ? std::to_string(*options.cpu) : "auto (inherited)";
}

void printPdos(std::ostream &out, const std::vector<PdoMappingConfig> &pdos, bool rx, bool discover) {
    if (pdos.empty()) out << "  " << (rx ? "RxPDO" : "TxPDO") << ": none\n";
    for (const auto &pdo : pdos) {
        out << "  " << (rx ? "RxPDO" : "TxPDO") << " 0x" << std::hex << std::setfill('0')
            << std::setw(4) << pdo.index << std::dec << std::setfill(' ')
            << (rx ? " (SM2, master -> slave)" : " (SM3, slave -> master)") << '\n';
        if (pdo.entries.empty()) {
            out << (discover ? "    entries: pending SDO discovery\n" : "    entries: none\n");
            continue;
        }
        out << "    Object       Bits  Bytes  Name\n";
        for (const auto &e : pdo.entries) {
            out << "    0x" << std::hex << std::setfill('0') << std::setw(4) << e.index
                << ':' << std::setw(2) << unsigned(e.sub_index) << std::dec << std::setfill(' ')
                << std::setw(7) << unsigned(e.bit_length) << std::setw(7) << unsigned(e.bit_length) / 8U
                << "  " << e.name << (e.index == 0 ? " [padding]" : "") << '\n';
        }
    }
}
} // namespace

void printStartupConfiguration(const PdoBusConfig &config, const RuntimeOptions &effective,
                               const char *version, std::ostream &output, bool color) {
    const ConsoleStyle style{color};
    std::ostringstream out;
    out << style.heading() << "\n=== ROCOS IgH " << version << " | Startup ===" << style.reset() << '\n'
        << "[Configuration]\n  YAML : " << absolutePath(effective.config_path) << '\n'
        << "  Mode : " << (config.from_eni ? "ENI topology" : "inline PDO YAML") << '\n';
    if (config.from_eni) out << "  ENI  : " << absolutePath(config.eni_path) << '\n';
    out << "  Differences are marked with *; Config/default includes ENI and built-in defaults.\n"
        << "  " << std::left << std::setw(23) << "Setting" << std::setw(23) << "Config/default" << "  Effective\n";
    const auto row = [&](const char *key, const std::string &configured, const std::string &value) {
        out << "  " << std::left << std::setw(23) << key << std::setw(23) << configured
            << (configured == value ? "  " : "* ") << value << '\n';
    };
    const auto &base = config.master;
    row("id", std::to_string(base.master_id), std::to_string(effective.master_id));
    row("cycle [us]", std::to_string(base.period_us), std::to_string(effective.period_us));
    row("cpuidx", cpuName(base), cpuName(effective));
    row("dc", base.dc_enabled ? "on" : "off", effective.dc_enabled ? "on" : "off");
    row("preop_timeout_ms", std::to_string(base.preop_timeout_ms), std::to_string(effective.preop_timeout_ms));
    row("op_timeout_ms", std::to_string(base.op_timeout_ms), std::to_string(effective.op_timeout_ms));
    row("pdo_config_attempts", std::to_string(base.pdo_config_attempts), std::to_string(effective.pdo_config_attempts));
    out << std::right << style.heading() << "\n[Configured topology] " << config.slaves.size()
        << " slave(s); positions are zero-based" << style.reset() << '\n';
    for (const auto &slave : config.slaves) {
        out << style.heading() << "Slave " << slave.id << ": " << slave.name << style.reset() << '\n';
        if (slave.vendor_id == 0 && slave.product_code == 0) out << "  Identity: discover from scanned SII\n";
        else out << "  Identity: vendor=0x" << std::hex << std::setfill('0') << std::setw(8) << slave.vendor_id
                 << " product=0x" << std::setw(8) << slave.product_code << std::dec << std::setfill(' ')
                 << " (strict match)\n";
        printPdos(out, slave.rx_pdos, true, !config.from_eni);
        printPdos(out, slave.tx_pdos, false, !config.from_eni);
        if (slave.dc) {
            const auto &dc = *slave.dc;
            out << "  DC: " << (effective.dc_enabled ? "enabled" : "disabled")
                << "  reference=" << (dc.reference ? "yes" : "no")
                << "  assign_activate=0x" << std::hex << std::setfill('0') << std::setw(4) << dc.assign_activate
                << std::dec << std::setfill(' ') << "  sync0_ns=";
            if (dc.sync0_cycle_ns) out << dc.sync0_cycle_ns;
            else out << std::uint64_t{effective.period_us} * 1000U << " (runtime period)";
            out << "  shift0_ns=" << dc.sync0_shift_ns << "  sync1_ns=" << dc.sync1_cycle_ns
                << "  shift1_ns=" << dc.sync1_shift_ns << '\n';
        } else out << "  DC: not configured\n";
        out << "  PREOP application SDOs: " << slave.init_commands.size() << '\n';
        for (const auto &cmd : slave.init_commands) {
            out << "    0x" << std::hex << std::setfill('0') << std::setw(4) << cmd.index
                << ':' << std::setw(2) << unsigned(cmd.sub_index) << "  data[LE]=";
            for (unsigned int i = 0; i < cmd.size; ++i)
                out << (i ? " " : "") << std::setw(2) << unsigned(cmd.data[i]);
            out << std::dec << std::setfill(' ') << " (" << unsigned(cmd.size) << " bytes)\n";
        }
    }
    output << out.str() << std::flush;
}

#if ROCOS_IGH_BUILD_MASTER
bool printBusScan(ec_master_t *master, unsigned int master_id, std::size_t configured,
                  std::ostream &output, bool color) {
    const ConsoleStyle style{color};
    std::ostringstream out;
    ec_master_info_t info{};
    const int rc = ecrt_master(master, &info);
    if (rc != 0) {
        out << style.warning() << "\n[Bus scan] master=" << master_id << " snapshot unavailable (rc=" << rc
            << "); scanned count is unknown" << style.reset() << '\n';
    } else {
        const bool healthy = info.link_up && !info.scan_busy && info.slave_count == configured;
        out << (healthy ? style.ok() : style.warning()) << "\n[Bus scan] master=" << master_id
            << " scanned=" << info.slave_count << " configured=" << configured
            << " link=" << (info.link_up ? "UP" : "DOWN")
            << " scan=" << (info.scan_busy ? "busy (snapshot may be incomplete)" : "complete") << style.reset() << '\n';
        if (info.slave_count != configured)
            out << style.warning() << "  WARNING: scanned/configured slave counts differ" << style.reset() << '\n';
        out << "  Pos   AL     Err  Vendor      Product     Revision    Serial      Scanned name\n";
        for (unsigned int i = 0; i < info.slave_count && i <= std::numeric_limits<std::uint16_t>::max(); ++i) {
            ec_slave_info_t slave{};
            const int slave_rc = ecrt_master_get_slave(master, static_cast<std::uint16_t>(i), &slave);
            if (slave_rc != 0) {
                out << style.warning() << "  " << i << "  information unavailable (rc=" << slave_rc << ')'
                    << style.reset() << '\n';
                continue;
            }
            const char *al = "UNKNOWN";
            switch (slave.al_state) {
            case EC_AL_STATE_INIT: al = "INIT"; break;
            case EC_AL_STATE_PREOP: al = "PREOP"; break;
            case EC_AL_STATE_SAFEOP: al = "SAFEOP"; break;
            case EC_AL_STATE_OP: al = "OP"; break;
            }
            out << (slave.error_flag ? style.error() : "") << "  " << std::dec << std::setfill(' ')
                << std::setw(3) << i << "  " << std::left << std::setw(7) << al << std::right
                << std::setw(3) << unsigned(slave.error_flag);
            for (const auto id : {slave.vendor_id, slave.product_code, slave.revision_number, slave.serial_number})
                out << "  0x" << std::hex << std::setfill('0') << std::setw(8) << id;
            out << std::dec << std::setfill(' ') << "  "
                << (slave.name[0] ? std::string(slave.name, strnlen(slave.name, sizeof(slave.name))) : "<unnamed>")
                << style.reset() << '\n';
        }
    }
    output << out.str() << std::flush;
    return rc == 0 && !info.scan_busy;
}
#endif
} // namespace rocos
