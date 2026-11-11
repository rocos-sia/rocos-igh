#include "startup_report.hpp"
#include "console_style.hpp"
#include "shared_memory_config.hpp"

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

} // namespace

void printStartupConfiguration(const PdoBusConfig &config, const RuntimeOptions &effective,
                               const char *version, std::ostream &output, bool color) {
    const ConsoleStyle style{color};
    std::ostringstream out;
    const auto row = [&](const char *key, const std::string &value) {
        out << "  " << std::left << std::setw(25) << key << " : " << value << '\n';
    };
    const auto setting = [&](const char *key, const std::string &configured, const std::string &value) {
        row(key, value + (configured == value ? "" : " (CLI override; config/default=" + configured + ")"));
    };
    out << style.heading() << "\n[Startup] ROCOS EtherCAT Master ==================================\n" << style.reset();
    row("version", version);
    row("backend", "igh (IgH EtherCAT Master; backend_type=2)");
#if ROCOS_IGH_BUILD_MASTER
    row("SDK", "IgH ecrt API " + std::to_string(ECRT_VER_MAJOR) + "." +
        std::to_string(ECRT_VER_MINOR) + " (compile-time headers)");
#else
    row("SDK", "unavailable (offline build)");
#endif
    row("config", absolutePath(effective.config_path));
    out << style.heading() << "[Config] Loaded settings (defaults included) --------------------\n" << style.reset();
    row("configuration source", config.from_eni ? "ENI topology" : "inline PDO YAML");
    row("master.eni", config.from_eni ? absolutePath(config.eni_path) : "<empty> (inline PDO YAML)");
    const auto &base = config.master;
    setting("master.id", std::to_string(base.master_id) + " (IgH / IPC namespace)",
            std::to_string(effective.master_id) + " (IgH / IPC namespace)");
    setting("master.cycle", std::to_string(base.period_us) + " us", std::to_string(effective.period_us) + " us");
    setting("master.cpuidx", cpuName(base), cpuName(effective));
    setting("master.dc", base.dc_enabled ? "on" : "off", effective.dc_enabled ? "on" : "off");
    setting("master.preop_timeout_ms", std::to_string(base.preop_timeout_ms) + " ms", std::to_string(effective.preop_timeout_ms) + " ms");
    setting("master.op_timeout_ms", std::to_string(base.op_timeout_ms) + " ms", std::to_string(effective.op_timeout_ms) + " ms");
    setting("master.pdo_config_attempts", std::to_string(base.pdo_config_attempts), std::to_string(effective.pdo_config_attempts));
    row("configured slaves", std::to_string(config.slaves.size()));
    out << std::right;
    for (const auto &slave : config.slaves) {
        out << style.heading() << "[Config] Slave[" << slave.id << "] " << slave.name << style.reset() << '\n';
        if (slave.vendor_id == 0 && slave.product_code == 0) out << "  Identity: discover from scanned SII\n";
        else out << "  Identity: vendor=0x" << std::hex << std::setfill('0') << std::setw(8) << slave.vendor_id
                 << " product=0x" << std::setw(8) << slave.product_code << std::dec << std::setfill(' ')
                 << " (strict match)\n";
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
    out << style.heading() << "[SHM] rocos-shm protocol ----------------------------------------\n" << style.reset();
    row("ABI version", std::to_string(SHM_ABI_VERSION));
    row("header / bus size", std::to_string(sizeof(ShmHeader)) + " / " + std::to_string(sizeof(EcatBus)) + " bytes");
    row("capacity", std::to_string(MAX_SLAVE_NUM) + " slaves; " + std::to_string(MAX_PDINPUT_NUM) +
        " Input / " + std::to_string(MAX_PDOUTPUT_NUM) + " Output variables per slave");
    const auto id = std::to_string(effective.master_id);
    row("bus", "/" + std::string(EC_SHM) + id);
    row("PDO Input / Output", "/pd_input_v2_" + id + " / /pd_output_v2_" + id);
    row("sync", "/" + std::string(EC_SEM_MUTEX) + id + "_{0.." + std::to_string(EC_SEM_NUM - 1) + "}");
    out << "================================================================\n"
        << style.ok() << "\n[License] AUTHORIZED - 已授权：可连续运行。\n"
        << "          IgH backend: is_authorized=true (license key not required).\n" << style.reset();
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
