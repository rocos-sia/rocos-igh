#include "process_data_layout.hpp"
#include "shared_memory_config.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <vector>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return false; } } while (false)
namespace {
using rocos::PdoDirection;
struct Fixture {
    rocos::SlaveSpec slave{};
    rocos::StaticSlaveConfig config{&slave, 1};
    explicit Fixture(std::vector<rocos::PdoEntrySpec> &entries) {
        slave.name = "Byte PDO";
        slave.syncs = reinterpret_cast<const ec_sync_info_t *>(1);
        slave.entries = entries.data(); slave.entry_count = entries.size();
    }
};

bool allWidthsAndOffsets() {
    for (unsigned int width = 1; width <= 255; ++width) {
        for (unsigned int shift = 0; shift < 8; ++shift) {
            std::vector<rocos::PdoEntrySpec> entries{
                {"In", PdoDirection::Input, 0x6000, 1, static_cast<std::uint8_t>(width), 1, shift},
                {"Out", PdoDirection::Output, 0x7000, 1, static_cast<std::uint8_t>(width), 1, shift}};
            Fixture f(entries);
            rocos::ProcessDataLayout layout;
            std::string error;
            const auto size = 1 + (shift + width + 7) / 8;
            const bool aligned = shift == 0 && width % 8 == 0;
            CHECK(layout.build(f.config, size, size, error) == aligned);
            rocos::EcatBus bus;
            CHECK(rocos::publishConfig(bus, f.config, error) == aligned);
            if (aligned) {
                CHECK(layout.inputSize() == size && layout.outputSize() == size);
                CHECK(bus.slaves[0].input_vars[0].offset == 1);
                CHECK(bus.slaves[0].input_vars[0].size == static_cast<int>(width / 8));
            } else CHECK(error.find("byte-aligned") != std::string::npos);
        }
    }
    return true;
}

bool metadataAndClients() {
    static_assert(sizeof(rocos::PdVar) == 84);
    static_assert(sizeof(rocos::Slave) == 4396);
    static_assert(sizeof(rocos::EcatBus) == 220088);
    static_assert(offsetof(rocos::SharedMemoryV2, bus) == 80);
    std::vector<rocos::PdoEntrySpec> entries{
        {"In", PdoDirection::Input, 0x6000, 1, 16, 1, 0},
        {"Out", PdoDirection::Output, 0x7000, 1, 16, 1, 0}};
    Fixture f(entries);
    rocos::ProcessDataLayout layout;
    std::string error;
    CHECK(layout.build(f.config, 3, 3, error));
    const int id = 1100000 + int(getpid()) * 2;
    rocos::SharedMemoryConfig owner(id), other(id + 1);
    CHECK(owner.createSharedMemory() && other.createSharedMemory());
    CHECK(owner.createPdDataMemoryProvider(3, 3) && other.createPdDataMemoryProvider(3, 3));
    CHECK(rocos::publishConfig(*owner.ecatBus, f.config, error));
    CHECK(rocos::publishConfig(*other.ecatBus, f.config, error));
    rocos::SharedMemoryConfig client(id), second(id + 1);
    CHECK(!client.connect()); // Partial metadata must never be visible.
    CHECK(owner.publishReady() && other.publishReady());
    CHECK(client.connect() && second.connect());
    owner.setSlaveInputVarValue<std::uint16_t>(0, 0, 0x1234);
    CHECK(client.getSlaveInputVarValue<std::uint16_t>(0, 0) == 0x1234);
    client.setSlaveOutputVarValue<std::uint16_t>(0, 0, 0xbeef);
    CHECK(owner.getSlaveOutputVarValue<std::uint16_t>(0, 0) == 0xbeef);
    CHECK(second.getSlaveOutputVarValue<std::uint16_t>(0, 0) == 0);
    bool rejected = false;
    try { (void)client.getSlaveInputVarPtr<std::uint16_t>(0, 0); }
    catch (const std::out_of_range &) { rejected = true; }
    CHECK(rejected); // Unaligned byte offsets use memcpy value access only.
    return true;
}

bool boundsTests() {
    std::vector<rocos::PdoEntrySpec> entries{{"In", PdoDirection::Input, 0x6000, 1, 16, 0, 0}};
    Fixture f(entries);
    rocos::ProcessDataLayout layout;
    std::string error;
    CHECK(!layout.build(f.config, 1, 0, error));
    CHECK(layout.build(f.config, 2, 0, error));
    entries[0].offset = 2;
    CHECK(!layout.build(f.config, 2, 0, error));
    CHECK(layout.inputSize() == 2);
    CHECK(!layout.build(f.config, EC_SHM_MAX_SIZE + 1, 0, error));
    layout.reset();
    CHECK(layout.inputSize() == 0 && layout.outputSize() == 0);
    return true;
}
} // namespace
int main() { return allWidthsAndOffsets() && metadataAndClients() && boundsTests() ? 0 : 1; }
