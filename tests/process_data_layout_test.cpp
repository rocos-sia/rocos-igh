#include "process_data_layout.hpp"
#include "shared_memory_config.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <vector>
#include <unistd.h>

namespace { bool realtime = false; unsigned int allocations = 0; }
void *operator new(std::size_t size) {
    if (realtime) ++allocations;
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return false; } } while (false)
namespace {
using rocos::PdoDirection;
struct Fixture {
    rocos::SlaveSpec slave{};
    rocos::StaticSlaveConfig config{&slave, 1};
    explicit Fixture(std::vector<rocos::PdoEntrySpec> &entries) {
        slave.name = "Packed GPIO";
        slave.syncs = reinterpret_cast<const ec_sync_info_t *>(1); // Layout never dereferences syncs.
        slave.entries = entries.data(); slave.entry_count = entries.size();
    }
};

bool allWidthsAndOffsets() {
    // Independent bit-by-bit oracle for every legal width and wire bit offset.
    for (unsigned int width = 1; width <= 255; ++width) {
        for (unsigned int shift = 0; shift < 8; ++shift) {
            std::vector<rocos::PdoEntrySpec> entries{
                {"In", PdoDirection::Input, 0x6000, 1, static_cast<std::uint8_t>(width), 1, shift},
                {"Out", PdoDirection::Output, 0x7000, 1, static_cast<std::uint8_t>(width), 1, shift}};
            Fixture f(entries);
            const auto size = 1 + (shift + width + 7) / 8;
            rocos::ProcessDataLayout layout;
            std::string error;
            CHECK(layout.build(f.config, size, size, error));
            std::vector<std::uint8_t> wire(size + 2, 0xa5), shared(layout.inputSize() + 2, 0xee);
            auto *domain = wire.data() + 1;
            auto *image = shared.data() + 1;
            for (unsigned int i = 0; i < size; ++i) domain[i] = static_cast<std::uint8_t>(i * 73 + width);
            realtime = true;
            std::memcpy(image, domain, size);
            layout.unpackInputs(domain, image);
            realtime = false;
            const auto offset = entries[0].shared_offset;
            for (unsigned int bit = 0; bit < (width + 7) / 8 * 8; ++bit) {
                const unsigned int expected = bit < width ?
                    ((domain[1 + (shift + bit) / 8] >> ((shift + bit) % 8)) & 1U) : 0U;
                CHECK(((image[offset + bit / 8] >> (bit % 8)) & 1U) == expected);
            }
            CHECK(shared.front() == 0xee && shared.back() == 0xee);
            CHECK(wire.front() == 0xa5 && wire.back() == 0xa5);
            std::vector<std::uint8_t> output(layout.outputSize(), 0xa5);
            const auto out_offset = entries[1].shared_offset;
            for (unsigned int byte = 0; byte < (width + 7) / 8; ++byte)
                output[out_offset + byte] = static_cast<std::uint8_t>(0x5a + byte);
            std::vector<std::uint8_t> expected(output.begin(), output.begin() + size);
            for (unsigned int bit = 0; bit < width; ++bit) {
                const unsigned int at = 8 + shift + bit;
                const unsigned int mask = 1U << (at % 8);
                const unsigned int value = (output[out_offset + bit / 8] >> (bit % 8)) & 1U;
                expected[at / 8] = static_cast<std::uint8_t>((expected[at / 8] & ~mask) | (value ? mask : 0));
            }
            realtime = true;
            std::memcpy(domain, output.data(), size);
            layout.packOutputs(output.data(), domain);
            realtime = false;
            CHECK(std::memcmp(expected.data(), domain, size) == 0);
            CHECK(wire.front() == 0xa5 && wire.back() == 0xa5);
            if (shift == 0 && width % 8 == 0)
                CHECK(layout.inputSize() == size && entries[0].shared_offset == 1);
        }
    }
    CHECK(allocations == 0);
    return true;
}

bool metadataAndClients() {
    // Original client descriptor layout remains unchanged.
    static_assert(sizeof(rocos::PdVar) == 84);
    static_assert(offsetof(rocos::PdVar, offset) == 72 && offsetof(rocos::PdVar, size) == 76);
    static_assert(offsetof(rocos::PdVar, index) == 80 && offsetof(rocos::PdVar, sub_index) == 82);
    std::vector<rocos::PdoEntrySpec> entries;
    for (const auto direction : {PdoDirection::Input, PdoDirection::Output}) {
        for (unsigned int bit = 0; bit < 8; ++bit)
            entries.push_back({"GPIO", direction, 0x7000, static_cast<std::uint8_t>(bit + 1), 1, 0, bit});
        entries.push_back({"Word", direction, 0x7010, 0, 16, 2, 0});
    }
    Fixture f(entries);
    rocos::ProcessDataLayout layout;
    rocos::EcatBus bus;
    std::string error;
    CHECK(!rocos::publishConfig(bus, f.config, error)); // Never publish overlapping byte aliases.
    CHECK(layout.build(f.config, 4, 4, error));
    CHECK(rocos::publishConfig(bus, f.config, error));
    CHECK(bus.slaves[0].input_var_num == 9 && bus.slaves[0].output_var_num == 9);
    CHECK(bus.slaves[0].input_vars[8].offset == 2 && bus.slaves[0].input_vars[8].size == 2);
    const int id = 1100000 + int(getpid()) * 2;
    rocos::SharedMemoryConfig owner(id), other(id + 1);
    CHECK(owner.createSharedMemory() && other.createSharedMemory());
    CHECK(owner.createPdDataMemoryProvider(layout.inputSize(), layout.outputSize()));
    CHECK(other.createPdDataMemoryProvider(layout.inputSize(), layout.outputSize()));
    *owner.ecatBus = bus; *other.ecatBus = bus;
    rocos::SharedMemoryConfig client(id), second(id + 1);
    CHECK(client.getSharedMemory() && client.getPdDataMemoryProvider());
    CHECK(second.getSharedMemory() && second.getPdDataMemoryProvider());
    std::array<std::uint8_t,4> input{0x69,0xaa,0x34,0x12}, output{};
    std::memcpy(owner.pdInputPtr, input.data(), input.size());
    layout.unpackInputs(input.data(), owner.pdInputPtr);
    for (unsigned int bit = 0; bit < 8; ++bit) {
        CHECK(bus.slaves[0].input_vars[bit].size == 1);
        CHECK(client.getSlaveInputVarValue<std::uint8_t>(0, bit) == ((0x69 >> bit) & 1));
        CHECK(second.getSlaveInputVarValue<std::uint8_t>(0, bit) == 0);
        client.setSlaveOutputVarValue<std::uint8_t>(0, bit, (0x96 >> bit) & 1);
    }
    CHECK(client.getSlaveInputVarValue<std::uint16_t>(0, 8) == 0x1234);
    client.setSlaveOutputVarValue<std::uint16_t>(0, 8, 0xbeef);
    std::memcpy(output.data(), owner.pdOutputPtr, output.size());
    layout.packOutputs(owner.pdOutputPtr, output.data());
    CHECK(output[0] == 0x96 && output[2] == 0xef && output[3] == 0xbe);
    CHECK(second.getSlaveOutputVarValue<std::uint16_t>(0, 8) == 0);
    return true;
}

bool boundsTests() {
    std::vector<rocos::PdoEntrySpec> entries{{"In", PdoDirection::Input, 0x6000, 1, 9, 0, 7}};
    Fixture f(entries);
    rocos::ProcessDataLayout layout;
    std::string error;
    CHECK(!layout.build(f.config, 1, 0, error));
    CHECK(entries[0].shared_offset == std::numeric_limits<unsigned int>::max());
    CHECK(layout.build(f.config, 2, 0, error));
    const auto previous_size = layout.inputSize();
    entries[0].offset = 2;
    CHECK(!layout.build(f.config, 2, 0, error));
    CHECK(layout.inputSize() == previous_size);
    entries[0].offset = 0; entries[0].bit_position = 8;
    CHECK(!layout.build(f.config, 2, 0, error));
    entries[0].bit_position = 0; entries[0].bit_length = 0;
    CHECK(!layout.build(f.config, 2, 0, error));
    entries[0].bit_length = 1;
    CHECK(!layout.build(f.config, EC_SHM_MAX_SIZE, 0, error));
    layout.reset();
    CHECK(layout.inputSize() == 0 && layout.outputSize() == 0);
    layout.unpackInputs(nullptr, nullptr); layout.packOutputs(nullptr, nullptr);
    return true;
}
} // namespace
int main() { return allWidthsAndOffsets() && metadataAndClients() && boundsTests() ? 0 : 1; }
