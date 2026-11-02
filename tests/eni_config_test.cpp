#include "pdo_config.hpp"
#include "slave_config.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; return false; \
} } while (false)

namespace {
struct Files {
    std::filesystem::path dir;
    Files() {
        char path[] = "/tmp/rocos_igh_eni_XXXXXX";
        if (auto *created = mkdtemp(path)) dir = created;
        else throw std::runtime_error("mkdtemp failed");
    }
    ~Files() { std::filesystem::remove_all(dir); }
    std::string write(const std::string &name, const std::string &text) {
        const auto path = dir / name;
        std::ofstream(path) << text;
        return path.string();
    }
};

std::string read(const std::string &path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

std::string replace(std::string text, const std::string &from, const std::string &to) {
    const auto pos = text.find(from);
    if (pos == std::string::npos) throw std::runtime_error("test replacement not found: " + from);
    text.replace(pos, from.size(), to);
    return text;
}

bool tests() {
    const auto example = std::string(ROCOS_IGH_SOURCE_DIR) + "/config/igh.yaml";
    const auto xml = read(std::string(ROCOS_IGH_SOURCE_DIR) + "/config/eni.xml");
    rocos::PdoBusConfig config;
    std::string error;
    CHECK(rocos::loadPdoConfig(example, config, error));
    CHECK(config.from_eni && config.source_yaml.empty());
    CHECK(config.master.period_us == 1000 && config.master.dc_enabled);
    CHECK(config.slaves.size() == 1);
    const auto &s = config.slaves[0];
    CHECK(s.id == 0 && s.vendor_id == 154 && s.product_code == 198948);
    CHECK(s.rx_pdos.size() == 4 && s.tx_pdos.size() == 5);
    CHECK(s.tx_pdos[0].index == 0x1a02); // Unselected 0x1a00 is not imported.
    CHECK(s.tx_pdos[0].entries[4].index == 0 && s.tx_pdos[0].entries[4].name == "Padding");
    CHECK(s.dc && s.dc->reference && s.dc->assign_activate == 0x0300 && s.dc->sync0_cycle_ns == 1000000);
    CHECK(s.init_commands.size() == 1 && s.init_commands[0].index == 0x6060 && s.init_commands[0].data[0] == 8);
#if ROCOS_IGH_BUILD_MASTER
    rocos::LoadedSlaveConfig loaded;
    CHECK(loaded.build(config, error));
    const auto view = loaded.view();
    CHECK(view.slaves[0].vendor_id == 154 && view.slaves[0].product_code == 198948);
    CHECK(view.slaves[0].syncs[0].n_pdos == 4 && view.slaves[0].syncs[1].n_pdos == 5);
    rocos::DistributedClockRuntimeConfig clocks;
    CHECK(rocos::buildDistributedClockRuntimeConfig(view, true, 1000, clocks, error));
    CHECK(!rocos::buildDistributedClockRuntimeConfig(view, true, 2000, clocks, error));
    CHECK(error.find("ENI DC CycleTime0") != std::string::npos);
#endif
    Files files;
    const auto file = files.write("bus.data", xml); // ENI references need no special suffix.
    CHECK(!rocos::loadPdoConfig(file, config, error));
    CHECK(error.find("--config requires YAML") != std::string::npos && config.slaves.empty());
    const std::string settings = "eni: bus.data\nid: 2\ncycle: 2000\ncpuidx: 1\ndc: off\n"
        "preop_timeout_ms: 30000\nop_timeout_ms: 90000\npdo_config_attempts: 8\n";
    const auto wrapper = files.write("igh.yaml", settings);
    CHECK(rocos::loadPdoConfig(wrapper, config, error));
    CHECK(config.master.master_id == 2 && config.master.cpu == 1 && !config.master.dc_enabled);
    CHECK(config.master.period_us == 2000);
    CHECK(config.master.preop_timeout_ms == 30000 && config.master.op_timeout_ms == 90000 &&
          config.master.pdo_config_attempts == 8);
    CHECK(config.from_eni && config.source_yaml.empty());
    char app[] = "master", flag[] = "--config", cpu[] = "--cpu", automatic[] = "auto";
    char *args[]{app, flag, const_cast<char *>(wrapper.c_str()), cpu, automatic};
    rocos::RuntimeOptions options;
    CHECK(rocos::parseRuntimeOptions(5, args, options, error, config.master));
    CHECK(options.master_id == 2 && !options.cpu);
    CHECK(options.period_us == 2000 && options.preop_timeout_ms == 30000);
    char id[] = "--master-id", zero[] = "0", cycle[] = "--period-us", period[] = "1000";
    char *overrides[]{app, flag, const_cast<char *>(wrapper.c_str()), id, zero, cycle, period};
    CHECK(rocos::parseRuntimeOptions(7, overrides, options, error, config.master));
    CHECK(options.master_id == 0 && options.period_us == 1000 && options.cpu == 1);
    CHECK(config.master.master_id == 2 && config.master.period_us == 2000);

    // As in ECM, the ENI wrapper has no required version or master block.
    CHECK(rocos::loadPdoConfig(files.write("defaults.yaml", "eni: bus.data\n"), config, error));
    CHECK(config.master.master_id == 0 && config.master.period_us == 1000 && !config.master.cpu && config.master.dc_enabled);
    CHECK(rocos::loadPdoConfig(files.write("null-cpu.yaml", "eni: bus.data\ncpuidx: null\n"), config, error));
    CHECK(!config.master.cpu);

    // XML order may differ from the physical wire order; AutoIncAddr is authoritative.
    const auto start = xml.find("<Slave>"), end = xml.find("</Slave>") + 8;
    const auto first = xml.substr(start, end - start);
    auto second = replace(first, "<AutoIncAddr>0</AutoIncAddr>", "<AutoIncAddr>65535</AutoIncAddr>");
    second = replace(second, "<PhysAddr>1001</PhysAddr>", "<PhysAddr>2005</PhysAddr>");
    second = replace(second, "<ReferenceClock>1</ReferenceClock>", "<ReferenceClock>0</ReferenceClock>");
    const auto two = replace(xml, first, second + first);
    CHECK(rocos::loadEniConfig(files.write("two.xml", two), config, error));
    CHECK(config.slaves.size() == 2 && config.slaves[0].dc->reference && !config.slaves[1].dc->reference);

    // SM assignment order can differ from the declaration catalogue order.
    auto ordered = replace(xml, "<Pdo>5634</Pdo>", "<Pdo>5643</Pdo>");
    const auto swap = ordered.find("<Pdo>5643</Pdo>", ordered.find("<Pdo>5643</Pdo>") + 1);
    ordered.replace(swap, std::string("<Pdo>5643</Pdo>").size(), "<Pdo>5634</Pdo>");
    CHECK(rocos::loadEniConfig(files.write("ordered.xml", ordered), config, error));
    CHECK(config.slaves[0].rx_pdos[0].index == 0x160b && config.slaves[0].rx_pdos[1].index == 0x1602);

    // Configurator emits explicit Su=0 and may omit unselected catalogue entries.
    const auto generated = replace(xml, "Sm=\"2\"", "Sm=\"2\" Su=\"0\"");
    CHECK(rocos::loadEniConfig(files.write("generated.xml", generated), config, error));
    const auto no_lists = replace(replace(xml, "<Pdo>5634</Pdo>", ""), "<Pdo>5643</Pdo>", "");
    // Exercise attribute-only selection by removing all SM2 assignment entries.
    auto attribute_only = replace(replace(no_lists, "<Pdo>5647</Pdo>", ""), "<Pdo>5660</Pdo>", "");
    CHECK(rocos::loadEniConfig(files.write("attributes.xml", attribute_only), config, error));
    CHECK(config.slaves[0].rx_pdos.size() == 4);

    const auto bad = [&](const std::string &text, const std::string &message) {
        if (rocos::loadEniConfig(files.write("bad.xml", text), config, error) ||
            !config.slaves.empty() || error.find(message) == std::string::npos) {
            std::cerr << "expected: " << message << "; got: " << error << '\n'; return false;
        }
        return true;
    };
    CHECK(bad(replace(xml, "<BitLen>8</BitLen>", "<BitLen>1</BitLen>"), "byte-aligned"));
    CHECK(bad(replace(xml, "<VendorId>154</VendorId>", "<VendorId>0</VendorId>"), "identity"));
    CHECK(bad(replace(xml, "<AutoIncAddr>0</AutoIncAddr>", "<AutoIncAddr>65535</AutoIncAddr>"), "AutoIncAddr"));
    CHECK(bad(replace(xml, "<Pdo>5634</Pdo>", "<Pdo>12345</Pdo>"), "conflicts"));
    CHECK(bad(replace(xml, "Sm=\"2\"", "Sm=\"4\""), "SM2"));
    CHECK(bad(replace(xml, "Sm=\"2\"", "Sm=\"2\" Su=\"1\""), "SyncUnit"));
    CHECK(bad(replace(xml, "<ReferenceClock>1</ReferenceClock>", "<ReferenceClock>0</ReferenceClock>"), "reference clock"));
    CHECK(bad(replace(xml, "<Data>0003</Data>", "<Data>0000</Data>"), "activation"));
    CHECK(bad(replace(xml, "<Ccs>1</Ccs>", "<Ccs>2</Ccs>"), "PS CoE"));
    CHECK(bad(replace(xml, "<Index>24672</Index>", "<Index>70000</Index>"), "Index"));
    CHECK(bad(replace(xml, "<Data>08</Data>", "<Data>ZZ</Data>"), "integer"));
    CHECK(bad(replace(xml, "<CoE>", "<CoE><InitCmds/><InitCmds/>"), "duplicated"));
    CHECK(bad(replace(xml, "<CycleTime>1000</CycleTime>", "<CycleTime>999</CycleTime>"), "1000"));
    CHECK(bad(replace(xml, "<Slave>", "<Slave><Info/>"), "duplicated"));
    CHECK(bad(replace(xml, "<EtherCATConfig", "<!DOCTYPE EtherCATConfig SYSTEM 'file:///tmp/eni-missing.dtd'><EtherCATConfig"), "DTDs"));
    CHECK(bad("<EtherCATConfig><Config>", "parse ENI"));
    const auto bad_yaml = [&](const std::string &text, const std::string &message) {
        if (rocos::loadPdoConfig(files.write("bad.yaml", text), config, error) ||
            !config.slaves.empty() || error.find(message) == std::string::npos) {
            std::cerr << "expected: " << message << "; got: " << error << '\n'; return false;
        }
        return true;
    };
    CHECK(bad_yaml("eni: bus.data\nslaves: []", "mutually exclusive"));
    CHECK(bad_yaml("eni: missing.xml", "missing.xml"));
    CHECK(bad_yaml(xml, "--config requires YAML")); // Filename alone cannot bypass the YAML entry point.
    CHECK(bad_yaml("eni: bus.data\nversion: 2", "version must be 1"));
    for (const std::string key : {"id", "cycle", "cpuidx", "preop_timeout_ms", "op_timeout_ms", "pdo_config_attempts"}) {
        for (const std::string invalid : {"-1", "+1", "1.5", "true", "[]", "\"1000\"", "4294967296"})
            CHECK(bad_yaml("eni: bus.data\n" + key + ": " + invalid, "root." + key));
        CHECK(bad_yaml("eni: bus.data\n" + key + ": 1000\n" + key + ": 2000", "root." + key + " is duplicated"));
    }
    CHECK(bad_yaml("eni: bus.data\ncycle: 999", "root.cycle"));
    CHECK(bad_yaml("eni: bus.data\ndc: true", "root.dc"));
    CHECK(bad_yaml("eni: bus.data\nmaster: {}\nid: 1", "cannot be combined"));
    for (const std::string key : {"dcmmode", "link", "license", "cycle_us", "master_id", "cpu"})
        CHECK(bad_yaml("eni: bus.data\n" + key + ": 1", "root." + key + " is not allowed"));
    CHECK(read(file) == xml); // Loading never writes ENI or the wrapper.
    CHECK(read(wrapper) == settings);
    return true;
}
} // namespace

int main(int argc, char **argv) {
    // Optional read-only inspection of external reference files; never reserves a master.
    if (argc == 2) {
        rocos::PdoBusConfig config;
        std::string error;
        if (!rocos::loadPdoConfig(argv[1], config, error)) { std::cerr << error << '\n'; return 1; }
#if ROCOS_IGH_BUILD_MASTER
        rocos::LoadedSlaveConfig loaded;
        if (!loaded.build(config, error)) { std::cerr << error << '\n'; return 1; }
#endif
        std::cout << "Loaded " << config.slaves.size() << " slaves; period_us=" << config.master.period_us
                  << "; dc=" << config.master.dc_enabled << '\n';
        return 0;
    }
    return tests() ? 0 : 1;
}
