#include "shared_memory_config.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; \
        return false; \
    } \
} while (false)

namespace {

int uniqueMasterId() {
    return 10000 + static_cast<int>(::getpid() % 10000);
}

bool testEcatBusDefaults() {
    const rocos::EcatBus bus{};
    CHECK(bus.current_state == ECAT_STATE_INIT);
    CHECK(bus.request_state == ECAT_STATE_OP);
    return true;
}

bool testInvalidArguments() {
    bool rejected = false;
    try {
        rocos::SharedMemoryConfig invalid(-1);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    CHECK(rejected);

    rocos::SharedMemoryConfig owner(uniqueMasterId());
    CHECK(owner.createSharedMemory());
    CHECK(!owner.createPdDataMemoryProvider(0, 8));
    CHECK(!owner.createPdDataMemoryProvider(8, -1));
    return true;
}

bool testMasterClientExchange() {
    const int id = uniqueMasterId();
    rocos::SharedMemoryConfig owner(id);
    CHECK(owner.createSharedMemory());
    CHECK(owner.createPdDataMemoryProvider(16, 16));

    rocos::SharedMemoryConfig client(id);
    CHECK(client.getSharedMemory());
    CHECK(client.getPdDataMemoryProvider());

    const std::uint32_t input = 0x12345678U;
    std::memcpy(owner.pdInputPtr, &input, sizeof(input));
    std::uint32_t observed = 0;
    std::memcpy(&observed, client.pdInputPtr, sizeof(observed));
    CHECK(observed == input);

    errno = 0;
    CHECK(sem_trywait(client.sem_mutex[0]) == -1);
    CHECK(errno == EAGAIN);
    CHECK(owner.notifyClients());
    CHECK(client.waitForSignal(0));
    CHECK(!client.waitForSignal(-1));
    CHECK(!client.waitForSignal(EC_SEM_NUM));
    return true;
}

bool testSecondOwnerIsolation() {
    const int id = uniqueMasterId();
    rocos::SharedMemoryConfig owner_b(id + 1);
    CHECK(owner_b.createSharedMemory());
    CHECK(owner_b.createPdDataMemoryProvider(16, 16));

    rocos::SharedMemoryConfig client_b(id + 1);
    CHECK(client_b.getSharedMemory());
    CHECK(client_b.getPdDataMemoryProvider());

    const std::uint32_t pattern_b = 0x55555555U;
    std::memcpy(owner_b.pdInputPtr, &pattern_b, sizeof(pattern_b));

    std::uint32_t observed_b = 0;
    std::memcpy(&observed_b, client_b.pdInputPtr, sizeof(observed_b));
    CHECK(observed_b == pattern_b);

    {
        rocos::SharedMemoryConfig owner_a(id);
        rocos::SharedMemoryConfig client_a(id);
        CHECK(owner_a.createSharedMemory());
        CHECK(owner_a.createPdDataMemoryProvider(16, 16));
        CHECK(client_a.getSharedMemory());
        CHECK(client_a.getPdDataMemoryProvider());

        const std::uint32_t pattern_a = 0xAAAAAAAAU;
        std::memcpy(owner_a.pdInputPtr, &pattern_a, sizeof(pattern_a));
        std::memcpy(owner_b.pdInputPtr, &pattern_b, sizeof(pattern_b));

        std::uint32_t observed_a = 0;
        observed_b = 0;
        std::memcpy(&observed_a, client_a.pdInputPtr, sizeof(observed_a));
        std::memcpy(&observed_b, client_b.pdInputPtr, sizeof(observed_b));
        CHECK(observed_a == pattern_a);
        CHECK(observed_b == pattern_b);
    }

    observed_b = 0;
    std::memcpy(&observed_b, client_b.pdInputPtr, sizeof(observed_b));
    CHECK(observed_b == pattern_b);
    return true;
}

}  // namespace

int main() {
    CHECK(testEcatBusDefaults());
    CHECK(testInvalidArguments());
    CHECK(testMasterClientExchange());
    CHECK(testSecondOwnerIsolation());
    return 0;
}
