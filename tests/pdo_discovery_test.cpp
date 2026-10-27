#include "pdo_discovery.hpp"
#include "slave_config.hpp"
#include "ethercat_master.hpp"

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <tuple>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return false; } } while (false)
namespace {
using Key = std::tuple<unsigned int, unsigned int, unsigned int>;
struct Value { std::uint32_t value; std::size_t size; };
std::map<Key, Value> dictionary;
std::vector<Key> reads;
bool unhealthy = false, late = false, change_file = false, change_count = false;
bool bad_state_after_read = false;
bool fail_rename = false;
unsigned int requested_master = 0, releases = 0, domains = 0, downloads = 0, activations = 0;
std::string changed_path;

std::string contents(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)), {}};
}
void put(const std::string &path, const std::string &text) {
    std::ofstream file(path, std::ios::binary); file << text;
}
struct File {
    std::string path;
    explicit File(const std::string &text) {
        char name[] = "/tmp/rocos_igh_discovery_XXXXXX";
        const int fd = mkstemp(name);
        if (fd >= 0) { close(fd); path = name; put(path, text); }
    }
    ~File() { if (!path.empty()) unlink(path.c_str()); }
};
std::string yaml(const std::string &empty = "{}") {
    return "# 保留原有注释\nversion: 1\nmaster: {period_us: 2000}\nslaves:\n"
        "  - id: 0\n    name: GPIO\n    init_commands: []\n    rx_pdos:\n"
        "      - index: 0x1600\n        entries: " + empty + " # Rx defaults\n"
        "      - index: 0x1601\n        entries: [{name: Existing, index: 0x6040, sub_index: 0, bit_length: 16}]\n"
        "    tx_pdos:\n      - index: 0x1A00\n        entries:\n";
}
void reset() {
    reads.clear(); unhealthy = late = change_file = change_count = bad_state_after_read = false;
    fail_rename = false;
    requested_master = releases = domains = downloads = activations = 0;
    dictionary = {{{0,0x1600,0},{2,1}}, {{0,0x1600,1},{0x70000110,4}},
                  {{0,0x1600,2},{0x70000220,4}}, {{0,0x1a00,0},{1,1}},
                  {{0,0x1a00,1},{0x60000108,4}}};
}
bool discover(rocos::PdoBusConfig &config, const std::string &path, std::string &error) {
    return rocos::discoverEmptyPdoEntries(reinterpret_cast<ec_master_t *>(1), config, path,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100), error);
}

bool successTests() {
    for (const std::string empty : {"{}", "[]", "null", "~", ""}) {
        reset();
        File file(yaml(empty));
        CHECK(chmod(file.path.c_str(), 0640) == 0);
        rocos::PdoBusConfig config;
        std::string error;
        CHECK(rocos::loadPdoConfig(file.path, config, error));
        CHECK(discover(config, file.path, error));
        CHECK(error.empty());
        const auto &rx = config.slaves[0].rx_pdos[0].entries;
        CHECK(rx.size() == 2 && rx[0].name == "RxPDO_1600_01" && rx[1].name == "RxPDO_1600_02");
        CHECK(rx[0].index == 0x7000 && rx[0].sub_index == 1 && rx[0].bit_length == 16);
        CHECK(rx[1].index == 0x7000 && rx[1].sub_index == 2 && rx[1].bit_length == 32);
        CHECK(config.slaves[0].tx_pdos[0].entries[0].name == "TxPDO_1A00_01");
        for (const auto &key : reads) CHECK(std::get<1>(key) != 0x1601);
        const auto saved = contents(file.path);
        CHECK(saved.find("# 保留原有注释\nversion: 1\nmaster: {period_us: 2000}") == 0);
        CHECK(saved.find("# Rx defaults") != std::string::npos);
        CHECK(saved.find("entries: [{name: Existing, index: 0x6040, sub_index: 0, bit_length: 16}]") != std::string::npos);
        CHECK(saved == config.source_yaml);
        struct stat info{};
        CHECK(stat(file.path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0640);
        rocos::PdoBusConfig reloaded;
        CHECK(rocos::loadPdoConfig(file.path, reloaded, error));
        CHECK(reloaded.slaves[0].rx_pdos[0].entries.size() == 2);
        CHECK(reloaded.master.period_us == 2000);
        rocos::LoadedSlaveConfig loaded;
        CHECK(loaded.build(reloaded, error));
        CHECK(loaded.view().slaves[0].entry_count == 4);
        reads.clear();
        CHECK(discover(reloaded, file.path, error));
        CHECK(reads.empty() && contents(file.path) == saved);
    }
    // Inline maps and nulls without a space after the colon remain valid YAML.
    reset();
    File inline_file("version: 1\nslaves: [{id: 0, name: GPIO, rx_pdos: [{index: 0x1600, entries: []}], tx_pdos: [{index: 0x1a00, entries: null}]}]\n");
    rocos::PdoBusConfig config;
    std::string error;
    CHECK(rocos::loadPdoConfig(inline_file.path, config, error));
    CHECK(discover(config, inline_file.path, error));
    CHECK(rocos::loadPdoConfig(inline_file.path, config, error));
    // A genuine zero mapping stays [], and is eligible for discovery next time.
    reset(); dictionary[{0,0x1600,0}] = {0,1}; dictionary[{0,0x1a00,0}] = {0,1};
    File zero(yaml());
    CHECK(rocos::loadPdoConfig(zero.path, config, error));
    CHECK(discover(config, zero.path, error));
    CHECK(config.slaves[0].rx_pdos[0].entries.empty());
    CHECK(contents(zero.path).find("entries: [] # Rx defaults") != std::string::npos);
    reset();
    File multiple(yaml() + "  - id: 1\n    name: Second\n    rx_pdos: [{index: 0x1600, entries: []}]\n"
        "    tx_pdos: [{index: 0x1a00, entries: {}}]\n");
    dictionary[{1,0x1600,0}] = {1,1}; dictionary[{1,0x1600,1}] = {0x71000308,4};
    dictionary[{1,0x1a00,0}] = {1,1}; dictionary[{1,0x1a00,1}] = {0x61000410,4};
    CHECK(rocos::loadPdoConfig(multiple.path, config, error));
    CHECK(discover(config, multiple.path, error));
    CHECK(config.slaves[0].rx_pdos[0].entries[0].index == 0x7000);
    CHECK(config.slaves[1].rx_pdos[0].entries[0].index == 0x7100);
    CHECK(config.slaves[1].tx_pdos[0].entries[0].sub_index == 4);
    CHECK(rocos::loadPdoConfig(multiple.path, config, error));
    CHECK(config.slaves[1].tx_pdos[0].entries[0].index == 0x6100);
    return true;
}

bool failureTests() {
    for (int fault = 0; fault < 12; ++fault) {
        reset();
        File file(yaml());
        const auto original = contents(file.path);
        rocos::PdoBusConfig config;
        std::string error;
        CHECK(rocos::loadPdoConfig(file.path, config, error));
        switch (fault) {
        case 0: dictionary.erase({0,0x1a00,1}); break; // Late failure: don't save partial Rx data.
        case 1: dictionary[{0,0x1600,0}].size = 2; break;
        case 2: dictionary[{0,0x1600,1}].size = 2; break;
        case 3: dictionary[{0,0x1600,1}].value = 0; break;
        case 4: dictionary[{0,0x1600,1}].value = 0x70000100; break;
        case 5: dictionary[{0,0x1600,0}].value = 25; break; // Existing entry uses one slot.
        case 6: dictionary[{0,0x1a00,0}].value = 26; break;
        case 7: unhealthy = true; break;
        case 8: late = true; break;
        case 9: change_file = true; changed_path = file.path; break;
        case 10: change_count = true; break;
        case 11: bad_state_after_read = true; break;
        }
        CHECK(!discover(config, file.path, error));
        CHECK(!error.empty());
        CHECK(config.slaves[0].rx_pdos[0].entries.empty());
        CHECK(config.slaves[0].tx_pdos[0].entries.empty());
        CHECK(config.source_yaml == original);
        CHECK(contents(file.path) == (fault == 9 ? original + "# concurrent edit\n" : original));
        if (fault == 0) CHECK(error.find("0x1a00:01") != std::string::npos && error.find("abort=0x6020000") != std::string::npos);
        if (fault == 8) CHECK(error.find("timeout") != std::string::npos);
        if (fault == 9) CHECK(error.find("changed") != std::string::npos);
    }
    // File replacement failure must not publish the in-memory candidate.
    reset();
    File file(yaml());
    rocos::PdoBusConfig config;
    std::string error;
    CHECK(rocos::loadPdoConfig(file.path, config, error));
    CHECK(!discover(config, file.path + "/missing", error));
    CHECK(config.slaves[0].rx_pdos[0].entries.empty());
    CHECK(contents(file.path) == yaml());
    fail_rename = true;
    CHECK(!discover(config, file.path, error));
    CHECK(error.find("cannot replace") != std::string::npos);
    CHECK(config.slaves[0].rx_pdos[0].entries.empty() && contents(file.path) == yaml());
    const auto prefix = std::filesystem::path(file.path).filename().string() + ".pdo-";
    for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::path(file.path).parent_path()))
        CHECK(entry.path().filename().string().find(prefix) != 0); // Failed writes leave no temporary file.
    return true;
}

bool packedMappingTests() {
    for (const unsigned int width : {1U, 7U, 9U, 12U, 16U, 31U, 64U, 255U}) {
        reset();
        // In particular, the reported AX58200 mapping 0x70000101 is one bit.
        dictionary[{0,0x1600,1}].value = 0x70000100U | width;
        dictionary[{0,0x1a00,1}].value = 0x60000100U | width;
        File file(yaml());
        rocos::PdoBusConfig config;
        std::string error;
        CHECK(rocos::loadPdoConfig(file.path, config, error));
        if (width % 8 != 0) {
            CHECK(!discover(config, file.path, error));
            CHECK(error.find("byte-aligned") != std::string::npos);
            CHECK(config.slaves[0].rx_pdos[0].entries.empty());
            CHECK(contents(file.path) == yaml());
            continue;
        }
        CHECK(discover(config, file.path, error));
        CHECK(config.slaves[0].rx_pdos[0].entries[0].bit_length == width);
        CHECK(rocos::loadPdoConfig(file.path, config, error));
        CHECK(config.slaves[0].rx_pdos[0].entries[0].bit_length == width);
        CHECK(config.slaves[0].rx_pdos[0].entries[0].index == 0x7000);
        CHECK(config.slaves[0].tx_pdos[0].entries[0].bit_length == width);
        rocos::LoadedSlaveConfig loaded;
        CHECK(loaded.build(config, error));
        CHECK(loaded.view().slaves[0].syncs[0].pdos[0].entries[0].bit_length == width);
    }
    return true;
}

bool startupOrderTests() {
    for (const bool fail_read : {false, true}) {
        reset();
        File file(yaml());
        if (fail_read) dictionary.erase({0,0x1a00,1});
        rocos::PdoBusConfig config;
        rocos::LoadedSlaveConfig loaded;
        rocos::EthercatMaster master;
        rocos::RuntimeOptions options;
        options.master_id = 42;
        options.config_path = file.path;
        options.preop_timeout_ms = 1000;
        std::string error;
        CHECK(rocos::loadPdoConfig(file.path, config, error));
        // Deliberately fail domain creation, after discovery and view rebuilding.
        CHECK(!master.initialize(options, config, loaded, error));
        CHECK(requested_master == 42 && releases == 1);
        CHECK(downloads == 0 && activations == 0);
        CHECK(!master.initialized());
        if (fail_read) {
            CHECK(domains == 0 && contents(file.path) == yaml());
        } else {
            CHECK(domains == 1 && error == "failed to create input domain");
            CHECK(loaded.view().slaves[0].entry_count == 4);
            CHECK(contents(file.path).find("RxPDO_1600_01") != std::string::npos);
        }
    }
    return true;
}
} // namespace

extern "C" int __real_rename(const char *, const char *);
extern "C" int __wrap_rename(const char *from, const char *to) {
    if (fail_rename) { errno = EACCES; return -1; }
    return __real_rename(from, to);
}
extern "C" ec_master_t *__wrap_ecrt_request_master(unsigned int id) {
    requested_master = id; return reinterpret_cast<ec_master_t *>(1);
}
extern "C" void __wrap_ecrt_release_master(ec_master_t *) { ++releases; }
extern "C" ec_domain_t *__wrap_ecrt_master_create_domain(ec_master_t *) { ++domains; return nullptr; }
extern "C" int __wrap_ecrt_master_activate(ec_master_t *) { ++activations; return -EPERM; }
extern "C" int __wrap_ecrt_master_sdo_download(ec_master_t *, std::uint16_t,
    std::uint16_t, std::uint8_t, const std::uint8_t *, std::size_t, std::uint32_t *) {
    ++downloads; return -EPERM;
}
extern "C" int __wrap_ecrt_master_get_slave(ec_master_t *, std::uint16_t, ec_slave_info_t *info) {
    *info = {};
    info->al_state = unhealthy || (bad_state_after_read && !reads.empty()) ? EC_AL_STATE_OP : EC_AL_STATE_PREOP;
    info->vendor_id = 1; info->product_code = 2;
    return 0;
}
extern "C" int __wrap_ecrt_master_sdo_upload(ec_master_t *, std::uint16_t position,
    std::uint16_t index, std::uint8_t sub, std::uint8_t *data, std::size_t capacity,
    std::size_t *size, std::uint32_t *abort_code) {
    const Key key{position,index,sub};
    reads.push_back(key);
    if (late) std::this_thread::sleep_for(std::chrono::milliseconds(150));
    if (change_file) { put(changed_path, contents(changed_path) + "# concurrent edit\n"); change_file = false; }
    if (change_count && reads.size() == 4) dictionary[key].value = 1;
    const auto found = dictionary.find(key);
    if (found == dictionary.end()) { *abort_code = 0x06020000; return -EIO; }
    *size = found->second.size;
    if (*size > capacity) return -EOVERFLOW;
    for (std::size_t i = 0; i < *size; ++i) data[i] = found->second.value >> (8 * i);
    return 0;
}

int main() { return successTests() && failureTests() && packedMappingTests() && startupOrderTests() ? 0 : 1; }
