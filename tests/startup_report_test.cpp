#include "startup_report.hpp"
#include "console_style.hpp"
#if ROCOS_IGH_BUILD_MASTER
#include "slave_config.hpp"
#include "process_data_layout.hpp"
#include "shared_memory_config.hpp"
#endif

#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; return false; \
} } while (false)

namespace {
rocos::PdoBusConfig example() {
    rocos::PdoBusConfig config;
    config.master.master_id = 2;
    config.from_eni = true;
    config.eni_path = "config/eni.xml";
    rocos::SlavePdoConfig slave;
    slave.name = "Configured Drive";
    slave.vendor_id = 154;
    slave.product_code = 198948;
    slave.rx_pdos = {{0x1600, {{"Control Word", 0x6040, 0, 16}, {"Mode", 0x6060, 0, 8}}}};
    slave.tx_pdos = {{0x1a00, {{"Position", 0x6064, 0, 32}, {"Padding", 0, 0, 8}}}};
    slave.dc = rocos::DistributedClockConfig{0x0300, -1000, 0, 0, true, 1000000};
    slave.init_commands = {{0x6060, 0, 1, {8}}};
    config.slaves.push_back(slave);
    return config;
}

bool configurationTests() {
    auto config = example();
    auto effective = config.master;
    effective.master_id = 3;
    effective.period_us = 2000;
    effective.cpu = 0;
    effective.config_path = "config/igh.yaml";
    std::ostringstream out;
    out << std::hex;
    const auto flags = out.flags();
    rocos::printStartupConfiguration(config, effective, "3.1.0-rc.2", out);
    CHECK(out.flags() == flags); // Formatting cannot leak into subsequent messages.
    const auto text = out.str();
    CHECK(text.find("[Startup] ROCOS EtherCAT Master") != std::string::npos);
    CHECK(text.find("version                   : 3.1.0-rc.2") != std::string::npos);
    CHECK(text.find("igh (IgH EtherCAT Master; backend_type=2)") != std::string::npos);
    CHECK(text.find("master.id                 : 3 (IgH / IPC namespace) (CLI override;") != std::string::npos);
    CHECK(text.find("master.cycle              : 2000 us (CLI override; config/default=1000 us)") != std::string::npos);
    CHECK(text.find("master.cpuidx             : 0 (CLI override;") != std::string::npos);
    for (const char *field : {"master.preop_timeout_ms", "master.op_timeout_ms", "master.pdo_config_attempts", "config                    : /", "master.eni                : /"})
        CHECK(text.find(field) != std::string::npos);
    CHECK(text.find("[Config] Slave[0] Configured Drive") != std::string::npos);
    CHECK(text.find("vendor=0x0000009a product=0x00030924") != std::string::npos);
    CHECK(text.find("DC: disabled") != std::string::npos && text.find("shift0_ns=-1000") != std::string::npos);
    CHECK(text.find("0x6060:00  data[LE]=08 (1 bytes)") != std::string::npos);
    CHECK(text.find("ABI version               : 2") != std::string::npos);
    CHECK(text.find("header / bus size") != std::string::npos);
    CHECK(text.find("/ecm_v2_3") != std::string::npos);
    CHECK(text.find("/pd_input_v2_3 / /pd_output_v2_3") != std::string::npos);
    CHECK(text.find("/sync_v2_3_{0..9}") != std::string::npos);
    CHECK(text.find("[License] AUTHORIZED - 已授权") != std::string::npos);
    CHECK(text.find("[Startup]") < text.find("[Config]"));
    CHECK(text.find("[Config]") < text.find("[SHM]"));
    CHECK(text.find("[SHM]") < text.find("[License]"));
    CHECK(text.find("[PDO]") == std::string::npos);
    CHECK(text.find('\033') == std::string::npos);
    CHECK(!rocos::consoleColorEnabled(out));
    config.from_eni = false;
    config.slaves[0].vendor_id = config.slaves[0].product_code = 0;
    std::ostringstream yaml;
    rocos::printStartupConfiguration(config, effective, "test", yaml, true);
    CHECK(yaml.str().find("<empty> (inline PDO YAML)") != std::string::npos);
    CHECK(yaml.str().find("discover from scanned SII") != std::string::npos);
    CHECK(yaml.str().find("\033[1;36m") != std::string::npos && yaml.str().find("\033[0m") != std::string::npos);
    CHECK(yaml.str().find("\033[1;32m") != std::string::npos);
    return true;
}

#if ROCOS_IGH_BUILD_MASTER
bool master_failure = false, scan_busy = false, slave_failure = false;
unsigned int scanned_count = 3;
std::vector<unsigned int> positions;

bool scanTests() {
    auto *master = reinterpret_cast<ec_master_t *>(1);
    std::ostringstream out;
    CHECK(rocos::printBusScan(master, 2, 1, out));
    CHECK(out.str().find("scanned=3 configured=1") != std::string::npos);
    CHECK(out.str().find("counts differ") != std::string::npos);
    CHECK(out.str().find("Actual Drive 2") != std::string::npos); // Includes extra/unconfigured slaves.
    CHECK(out.str().find("PREOP") != std::string::npos && out.str().find("0x0000009a") != std::string::npos);
    CHECK(positions == std::vector<unsigned int>({0, 1, 2}));
    CHECK(out.str().find('\033') == std::string::npos);
    positions.clear();
    master_failure = true;
    std::ostringstream failed;
    CHECK(!rocos::printBusScan(master, 2, 1, failed));
    CHECK(failed.str().find("scanned count is unknown") != std::string::npos);
    CHECK(positions.empty()); // Cannot label the configured count as a scanned result.
    master_failure = false;
    scan_busy = true;
    slave_failure = true;
    std::ostringstream busy;
    CHECK(!rocos::printBusScan(master, 2, 3, busy, true));
    CHECK(busy.str().find("snapshot may be incomplete") != std::string::npos);
    CHECK(busy.str().find("information unavailable (rc=-5)") != std::string::npos);
    CHECK(busy.str().find("Actual Drive 2") != std::string::npos);
    CHECK(busy.str().find("\033[1;33m") != std::string::npos);
    return true;
}
#endif
} // namespace

#if ROCOS_IGH_BUILD_MASTER
extern "C" int __wrap_ecrt_master(ec_master_t *, ec_master_info_t *info) {
    *info = {};
    info->slave_count = scanned_count;
    info->link_up = 1;
    info->scan_busy = scan_busy;
    return master_failure ? -5 : 0;
}
extern "C" int __wrap_ecrt_master_get_slave(ec_master_t *, std::uint16_t position, ec_slave_info_t *info) {
    positions.push_back(position);
    *info = {};
    info->al_state = EC_AL_STATE_PREOP;
    info->vendor_id = 154;
    info->product_code = 198948;
    const auto name = "Actual Drive " + std::to_string(position);
    std::memcpy(info->name, name.c_str(), name.size() + 1);
    return slave_failure && position == 1 ? -5 : 0;
}
#endif

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--preview") == 0) {
        const auto config = example();
        auto options = config.master;
        options.config_path = "config/igh.yaml";
        options.dc_enabled = true;
        rocos::printStartupConfiguration(config, options, "preview", std::cout, rocos::consoleColorEnabled(std::cout));
#if ROCOS_IGH_BUILD_MASTER
        scanned_count = 1;
        rocos::printBusScan(reinterpret_cast<ec_master_t *>(1), options.master_id, config.slaves.size(),
                            std::cout, rocos::consoleColorEnabled(std::cout));
        rocos::LoadedSlaveConfig loaded;
        std::string error;
        if (!loaded.build(config, error)) return 1;
        auto view = loaded.view();
        view.slaves[0].entries[1].offset = 2;
        view.slaves[0].entries[3].offset = 4;
        std::cout << "\n[SHM] Process image: Input=5 bytes; Output=3 bytes\n";
        rocos::printLoadedSlaveConfig(loaded, std::cout, rocos::consoleColorEnabled(std::cout), scanned_count);
        rocos::ProcessDataLayout layout;
        rocos::EcatBus bus{};
        if (!layout.build(view, 5, 3, error) || !rocos::publishConfig(bus, view, error)) return 1;
        std::cout << "[PDO] Mapping validated; end of PDO configuration.\n";
#endif
        return 0;
    }
    if (!configurationTests()) return 1;
#if ROCOS_IGH_BUILD_MASTER
    if (!scanTests()) return 1;
#endif
    return 0;
}
