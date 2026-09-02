#include "shared_memory_config.hpp"
#if ROCOS_IGH_BUILD_MASTER
#include "ethercat_master.hpp"
#include "slave_config.hpp"
#endif

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <atomic>
#include <type_traits>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <thread>
#include <vector>

#if ROCOS_IGH_BUILD_MASTER
namespace rocos {
struct EthercatMasterTestPeer {
    static void seedState(EthercatMaster &master,
                          bool initialized,
                          ec_master_t *master_ptr,
                          ec_domain_t *input_domain,
                          ec_domain_t *output_domain,
                          std::uint8_t *input_data,
                          std::uint8_t *output_data,
                          std::size_t input_size,
                          std::size_t output_size) {
        master.initialized_ = initialized;
        master.master_ = master_ptr;
        master.input_domain_ = input_domain;
        master.output_domain_ = output_domain;
        master.input_data_ = input_data;
        master.output_data_ = output_data;
        master.input_size_ = input_size;
        master.output_size_ = output_size;
    }
};
}  // namespace rocos
#endif

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

std::string sharedMemoryName(const std::string &prefix, int id) {
    return "/" + prefix + std::to_string(id);
}

std::string semaphoreName(int id, int index) {
    return "/" + std::string(EC_SEM_MUTEX) + std::to_string(id) + "_" + std::to_string(index);
}

bool sharedMemoryExists(const std::string &name) {
    const int fd = shm_open(name.c_str(), O_RDWR, 0);
    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
}

bool semaphoreExists(const std::string &name) {
    sem_t *const sem = sem_open(name.c_str(), 0);
    if (sem == SEM_FAILED) {
        return false;
    }
    CHECK(sem_close(sem) == 0);
    return true;
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
    CHECK(!owner.createPdDataMemoryProvider(EC_SHM_MAX_SIZE + 1, 8));
    return true;
}

bool testClientRequiresExistingOwner() {
    const int id = uniqueMasterId() + 1000;
    rocos::SharedMemoryConfig client(id);
    CHECK(!client.getSharedMemory());
    CHECK(!client.getPdDataMemoryProvider());
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

bool testDuplicateOwnerRejected() {
    const int id = uniqueMasterId();
    rocos::SharedMemoryConfig owner(id);
    CHECK(owner.createSharedMemory());
    CHECK(owner.createPdDataMemoryProvider(16, 16));

    rocos::SharedMemoryConfig client(id);
    CHECK(client.getSharedMemory());
    CHECK(client.getPdDataMemoryProvider());

    rocos::SharedMemoryConfig duplicate(id);
    CHECK(!duplicate.createSharedMemory());
    CHECK(!duplicate.createPdDataMemoryProvider(16, 16));

    const std::uint32_t pattern = 0xCAFEBABEU;
    std::memcpy(owner.pdInputPtr, &pattern, sizeof(pattern));

    std::uint32_t observed = 0;
    std::memcpy(&observed, client.pdInputPtr, sizeof(observed));
    CHECK(observed == pattern);
    return true;
}

bool testSemaphoreRollbackOnPartialFailure() {
    const int id = uniqueMasterId();
    const int blocked_index = EC_SEM_NUM - 1;
    const std::string blocked_name = semaphoreName(id, blocked_index);
    sem_t *const blocker = sem_open(blocked_name.c_str(), O_CREAT | O_EXCL, 0660, 0);
    CHECK(blocker != SEM_FAILED);

    rocos::SharedMemoryConfig owner(id);
    CHECK(!owner.createSharedMemory());
    CHECK(!sharedMemoryExists(sharedMemoryName(EC_SHM, id)));
    for (int index = 0; index < blocked_index; ++index) {
        CHECK(!semaphoreExists(semaphoreName(id, index)));
    }
    CHECK(semaphoreExists(blocked_name));

    CHECK(sem_close(blocker) == 0);
    CHECK(sem_unlink(blocked_name.c_str()) == 0);
    return true;
}

bool testPdRollbackOnPartialFailure() {
    const int id = uniqueMasterId();
    rocos::SharedMemoryConfig owner(id);
    CHECK(owner.createSharedMemory());

    const std::string blocked_output_name = sharedMemoryName("pd_output", id);
    const int blocker = shm_open(blocked_output_name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0660);
    CHECK(blocker >= 0);
    CHECK(ftruncate(blocker, 16) == 0);

    CHECK(!owner.createPdDataMemoryProvider(16, 16));
    CHECK(!sharedMemoryExists(sharedMemoryName("pd_input", id)));
    CHECK(sharedMemoryExists(blocked_output_name));

    CHECK(close(blocker) == 0);
    CHECK(shm_unlink(blocked_output_name.c_str()) == 0);
    return true;
}

bool testConcurrentWaitRegistration() {
    const int id = uniqueMasterId();
    rocos::SharedMemoryConfig owner(id);
    CHECK(owner.createSharedMemory());
    CHECK(owner.createPdDataMemoryProvider(16, 16));

    rocos::SharedMemoryConfig client(id);
    CHECK(client.getSharedMemory());
    CHECK(client.getPdDataMemoryProvider());

    std::atomic<int> ready{0};
    std::atomic<int> completed{0};
    std::vector<std::thread> waiters;
    for (int index = 0; index < 2; ++index) {
        waiters.emplace_back([&]() {
            ready.fetch_add(1, std::memory_order_relaxed);
            client.wait();
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    while (ready.load(std::memory_order_relaxed) < 2) {
        std::this_thread::yield();
    }

    CHECK(owner.notifyClients());
    for (auto &waiter : waiters) {
        waiter.join();
    }
    CHECK(completed.load(std::memory_order_relaxed) == 2);
    return true;
}

bool testWaitThreadLimitDoesNotDeadlock() {
    const int id = uniqueMasterId();
    rocos::SharedMemoryConfig owner(id);
    CHECK(owner.createSharedMemory());
    CHECK(owner.createPdDataMemoryProvider(16, 16));

    rocos::SharedMemoryConfig client(id);
    CHECK(client.getSharedMemory());
    CHECK(client.getPdDataMemoryProvider());

    std::atomic<int> ready{0};
    std::atomic<int> completed{0};
    std::vector<std::thread> waiters;
    for (int index = 0; index < EC_SEM_NUM + 1; ++index) {
        waiters.emplace_back([&]() {
            ready.fetch_add(1, std::memory_order_relaxed);
            client.wait();
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    while (ready.load(std::memory_order_relaxed) < EC_SEM_NUM + 1) {
        std::this_thread::yield();
    }

    CHECK(owner.notifyClients());
    for (auto &waiter : waiters) {
        waiter.join();
    }
    CHECK(completed.load(std::memory_order_relaxed) == EC_SEM_NUM + 1);
    return true;
}

#if ROCOS_IGH_BUILD_MASTER
static_assert(!std::is_copy_constructible<rocos::EthercatMaster>::value,
              "EthercatMaster must own one master exclusively");
static_assert(!std::is_move_constructible<rocos::EthercatMaster>::value,
              "domain pointers must not outlive their owner");

bool testPublishConfigMetadata() {
    static ec_sync_info_t sync_table[1] = {};
    static rocos::PdoEntrySpec entries[2] = {
        {"status", rocos::PdoDirection::Input, 0x6000, 1, 16, 12, 0},
        {"command", rocos::PdoDirection::Output, 0x7000, 2, 32, 20, 0},
    };
    static rocos::SlaveSpec slave = {
        0, 0, 0x00000002, 0x12345678, "synthetic", sync_table, entries, 2
    };
    rocos::StaticSlaveConfig config{&slave, 1};

    rocos::EcatBus bus{};
    bus.slave_num = 99;
    std::strncpy(bus.slaves[0].name, "stale", MAX_SLAVE_NAME_LEN - 1);

    std::string error;
    CHECK(rocos::publishConfig(bus, config, error));
    CHECK(error.empty());
    CHECK(bus.slave_num == 1);

    const rocos::Slave &published = bus.slaves[0];
    CHECK(published.id == 0);
    CHECK(std::string(published.name) == "synthetic");
    CHECK(published.input_var_num == 1);
    CHECK(published.output_var_num == 1);

    const rocos::PdVar &in = published.input_vars[0];
    CHECK(std::string(in.name) == "status");
    CHECK(in.offset == 12);
    CHECK(in.size == 2);
    CHECK(in.index == 0x6000);
    CHECK(in.sub_index == 1);

    const rocos::PdVar &out = published.output_vars[0];
    CHECK(std::string(out.name) == "command");
    CHECK(out.offset == 20);
    CHECK(out.size == 4);
    CHECK(out.index == 0x7000);
    CHECK(out.sub_index == 2);
    return true;
}

bool testPublishConfigMetadataBounds() {
    static ec_sync_info_t sync_table[1] = {};
    static rocos::PdoEntrySpec valid_entries[1] = {
        {"near_limit", rocos::PdoDirection::Input, 0x6001, 1, 16,
         static_cast<unsigned int>(EC_SHM_MAX_SIZE - 2), 0},
    };
    static rocos::SlaveSpec valid_slave = {
        0, 0, 0x00000002, 0x12345678, "valid", sync_table, valid_entries, 1
    };
    rocos::StaticSlaveConfig valid_config{&valid_slave, 1};

    rocos::EcatBus valid_bus{};
    std::string error;
    CHECK(rocos::publishConfig(valid_bus, valid_config, error));
    CHECK(error.empty());
    CHECK(valid_bus.slaves[0].input_vars[0].offset == EC_SHM_MAX_SIZE - 2);
    CHECK(valid_bus.slaves[0].input_vars[0].size == 2);

    static rocos::PdoEntrySpec overflow_entries[1] = {
        {"overflow", rocos::PdoDirection::Output, 0x7001, 1, 32,
         static_cast<unsigned int>(EC_SHM_MAX_SIZE - 2), 0},
    };
    static rocos::SlaveSpec overflow_slave = {
        0, 0, 0x00000002, 0x12345678, "overflow", sync_table, overflow_entries, 1
    };
    rocos::StaticSlaveConfig overflow_config{&overflow_slave, 1};

    rocos::EcatBus overflow_bus{};
    CHECK(!rocos::publishConfig(overflow_bus, overflow_config, error));
    CHECK(error.find("offset + size exceeds EC_SHM_MAX_SIZE") != std::string::npos);
    return true;
}

bool testMasterCyclicCallsBeforeInitialization() {
    rocos::EthercatMaster master;
    CHECK(!master.initialized());
    CHECK(master.inputData() == nullptr);
    CHECK(master.outputData() == nullptr);
    CHECK(master.inputSize() == 0);
    CHECK(master.outputSize() == 0);

    master.receiveAndProcess();
    master.queueAndSend();

    const rocos::BusState state = master.readState();
    CHECK(state.responding_slaves == 0U);
    CHECK(state.al_states == 0U);
    CHECK(!state.link_up);
    CHECK(state.input_working_counter == 0U);
    CHECK(state.output_working_counter == 0U);
    CHECK(state.input_wc_state == EC_WC_ZERO);
    CHECK(state.output_wc_state == EC_WC_ZERO);
    return true;
}

bool testInitializeRejectsAlreadyInitializedWithoutReset() {
    rocos::EthercatMaster master;
    auto *const seeded_input = reinterpret_cast<std::uint8_t *>(0x1000);
    auto *const seeded_output = reinterpret_cast<std::uint8_t *>(0x2000);
    auto *const seeded_input_domain = reinterpret_cast<ec_domain_t *>(0x3000);
    auto *const seeded_output_domain = reinterpret_cast<ec_domain_t *>(0x4000);

    rocos::EthercatMasterTestPeer::seedState(master,
                                             true,
                                             nullptr,
                                             seeded_input_domain,
                                             seeded_output_domain,
                                             seeded_input,
                                             seeded_output,
                                             64,
                                             128);

    std::string error;
    CHECK(!master.initialize(0, rocos::defaultSlaveConfig(), error));
    CHECK(error == "master already initialized");
    CHECK(master.initialized());
    CHECK(master.inputData() == seeded_input);
    CHECK(master.outputData() == seeded_output);
    CHECK(master.inputSize() == 64U);
    CHECK(master.outputSize() == 128U);

    rocos::EthercatMasterTestPeer::seedState(master,
                                             false,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             0,
                                             0);
    return true;
}

bool testInitializeFreshFailureResetsState() {
    rocos::EthercatMaster master;
    auto *const seeded_input = reinterpret_cast<std::uint8_t *>(0x5000);
    auto *const seeded_output = reinterpret_cast<std::uint8_t *>(0x6000);
    auto *const seeded_input_domain = reinterpret_cast<ec_domain_t *>(0x7000);
    auto *const seeded_output_domain = reinterpret_cast<ec_domain_t *>(0x8000);

    rocos::EthercatMasterTestPeer::seedState(master,
                                             false,
                                             nullptr,
                                             seeded_input_domain,
                                             seeded_output_domain,
                                             seeded_input,
                                             seeded_output,
                                             7,
                                             9);

    std::string error;
    const rocos::StaticSlaveConfig empty_config{};
    CHECK(!master.initialize(0, empty_config, error));
    CHECK(error == "no slave configuration compiled");
    CHECK(!master.initialized());
    CHECK(master.inputData() == nullptr);
    CHECK(master.outputData() == nullptr);
    CHECK(master.inputSize() == 0U);
    CHECK(master.outputSize() == 0U);
    return true;
}

bool testSlaveConfigValidation() {
    std::string error;
    CHECK(rocos::validateSlaveConfig(rocos::defaultSlaveConfig(), error));

    static const ec_sync_info_t sync_table[1] = {};
    rocos::PdoEntrySpec invalid_entry{
        "status", rocos::PdoDirection::Input, 0x6000, 1, 7, 0, 0
    };
    rocos::SlaveSpec invalid_slave{
        0, 0, 0x00000002, 0x12345678, "invalid", sync_table,
        &invalid_entry, 1
    };
    const rocos::StaticSlaveConfig invalid_config{&invalid_slave, 1};
    CHECK(!rocos::validateSlaveConfig(invalid_config, error));
    CHECK(error.find("byte-aligned") != std::string::npos);
    return true;
}
#endif

}  // namespace

int main() {
    if (!testEcatBusDefaults()) {
        return EXIT_FAILURE;
    }
    if (!testInvalidArguments()) {
        return EXIT_FAILURE;
    }
    if (!testClientRequiresExistingOwner()) {
        return EXIT_FAILURE;
    }
    if (!testMasterClientExchange()) {
        return EXIT_FAILURE;
    }
    if (!testSecondOwnerIsolation()) {
        return EXIT_FAILURE;
    }
    if (!testDuplicateOwnerRejected()) {
        return EXIT_FAILURE;
    }
    if (!testSemaphoreRollbackOnPartialFailure()) {
        return EXIT_FAILURE;
    }
    if (!testPdRollbackOnPartialFailure()) {
        return EXIT_FAILURE;
    }
    if (!testConcurrentWaitRegistration()) {
        return EXIT_FAILURE;
    }
    if (!testWaitThreadLimitDoesNotDeadlock()) {
        return EXIT_FAILURE;
    }
#if ROCOS_IGH_BUILD_MASTER
    if (!testPublishConfigMetadata()) {
        return EXIT_FAILURE;
    }
    if (!testPublishConfigMetadataBounds()) {
        return EXIT_FAILURE;
    }
    if (!testMasterCyclicCallsBeforeInitialization()) {
        return EXIT_FAILURE;
    }
    if (!testInitializeRejectsAlreadyInitializedWithoutReset()) {
        return EXIT_FAILURE;
    }
    if (!testInitializeFreshFailureResetsState()) {
        return EXIT_FAILURE;
    }
    if (!testSlaveConfigValidation()) {
        return EXIT_FAILURE;
    }
#endif
    return 0;
}
