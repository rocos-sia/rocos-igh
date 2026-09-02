# Minimal IgH EtherCAT Master Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the smallest C++17 IgH master runtime that supports one master per process, 1 ms cyclic PDO exchange, POSIX shared-memory IPC, semaphore notification, and independent multi-master instances.

**Architecture:** Each `rocos_igh_master` process owns one IgH master, one input domain, one output domain, and one `SharedMemoryConfig` selected by the same non-negative `master_id`. Slave and PDO definitions are compile-time C++ tables; this plan intentionally ships an empty default table plus a validated configuration interface because no target slave mapping has been supplied. The real-time loop performs only IgH `rt_safe` calls, fixed-size copies, bounded statistics, and non-blocking notification.

**Tech Stack:** C++17, CMake 3.16+, IgH EtherCAT userspace API (`ecrt.h`, `libethercat`), POSIX shared memory, POSIX named semaphores, pthreads, CTest.

## Global Constraints

- The minimum accepted period is exactly `1000 us`; 1 ms remains a deployment target rather than a guarantee on non-real-time Linux.
- Keep `EcatBus`, `Slave`, `PdVar`, `EcatConfigMaster`, and `EcatConfig` ABI-compatible.
- Preserve IPC names `ecm{id}`, `pd_input{id}`, `pd_output{id}`, and `sync{id}_{0..9}`; POSIX calls receive the leading `/`.
- Perform all allocation, slave configuration, PDO registration, activation, IPC mapping, and page prefaulting before entering the cyclic loop.
- The cyclic order is `receive -> process both domains -> publish input -> acquire output -> queue both domains -> send -> notify`.
- Do not add ROS, JSON/YAML parsing, RPC, GUI, database, hot reload, SDO management, automatic rescan, or advanced Distributed Clocks behavior.
- One process owns one `master_id`; one client may write `pd_output{id}`. Multi-entry output snapshots are best-effort until a future versioned IPC protocol adds transactional publication.
- Hardware tests are opt-in and never run in the default CTest set.
- Use [docs/architecture.md](../../architecture.md) for design decisions and [docs/api-guide.md](../../api-guide.md) for IgH phase and real-time API constraints.

## File Map

| Path | Responsibility |
|---|---|
| `CMakeLists.txt` | C++17 project, targets, options, dependencies, and CTest entry |
| `cmake/FindEtherCAT.cmake` | Locate `ecrt.h` and `libethercat`, expose `EtherCAT::EtherCAT` |
| `src/shared_memory_config.hpp` | Existing cross-process ABI and master/client IPC ownership |
| `src/slave_config.hpp` | Compile-time slave/PDO descriptors and validation contract |
| `src/slave_config.cpp` | Empty default configuration and non-real-time validation |
| `src/ethercat_master.hpp/.cpp` | RAII IgH master, two domains, configuration, state, receive/process/queue/send |
| `src/cyclic_task.hpp/.cpp` | Absolute-time scheduler, fixed-buffer exchange, statistics, notification |
| `src/runtime_options.hpp/.cpp` | Strict command-line parsing without EtherCAT side effects |
| `src/main.cpp` | Signal handling, startup ordering, real-time setup, run, and shutdown |
| `tests/shared_memory_config_test.cpp` | Single dependency-free test executable covering IPC, config, copies, deadlines, and options |

---

### Task 1: CMake and Dependency Discovery

**Files:**
- Modify: `CMakeLists.txt`
- Delete: `cmake/FindEtherCAT.make`
- Create: `cmake/FindEtherCAT.cmake`
- Create: `tests/shared_memory_config_test.cpp`

**Interfaces:**
- Consumes: system `ecrt.h`, `libethercat`, `Threads::Threads`, and `librt` where required.
- Produces: imported target `EtherCAT::EtherCAT`; interface target `rocos_igh_core`; executable target `shared_memory_config_test`; options `ROCOS_IGH_BUILD_MASTER` and `ROCOS_IGH_BUILD_HARDWARE_TESTS`.

- [ ] **Step 1: Write a compile smoke test for the existing shared ABI**

```cpp
#include "shared_memory_config.hpp"

#include <iostream>

int main() {
    const rocos::EcatBus bus{};
    if (bus.current_state != ECAT_STATE_INIT || bus.request_state != ECAT_STATE_OP) {
        std::cerr << "unexpected EcatBus defaults\n";
        return 1;
    }
    return 0;
}
```

- [ ] **Step 2: Verify the build has no configured targets yet**

Run: `cmake -S . -B build`

Expected: configuration does not provide a buildable `shared_memory_config_test` because the current top-level CMake file is empty.

- [ ] **Step 3: Add a conventional EtherCAT find module**

```cmake
find_path(EtherCAT_INCLUDE_DIR
    NAMES ecrt.h
    HINTS ENV ETHERCAT_ROOT
    PATH_SUFFIXES include
)
find_library(EtherCAT_LIBRARY
    NAMES ethercat
    HINTS ENV ETHERCAT_ROOT
    PATH_SUFFIXES lib lib64
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(EtherCAT
    REQUIRED_VARS EtherCAT_INCLUDE_DIR EtherCAT_LIBRARY
)

if(EtherCAT_FOUND AND NOT TARGET EtherCAT::EtherCAT)
    add_library(EtherCAT::EtherCAT UNKNOWN IMPORTED)
    set_target_properties(EtherCAT::EtherCAT PROPERTIES
        IMPORTED_LOCATION "${EtherCAT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${EtherCAT_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(EtherCAT_INCLUDE_DIR EtherCAT_LIBRARY)
```

- [ ] **Step 4: Add the minimal project and test target**

```cmake
cmake_minimum_required(VERSION 3.16)
project(rocos_igh VERSION 0.1.0 LANGUAGES CXX)

option(ROCOS_IGH_BUILD_MASTER "Build the IgH master executable" ON)
option(ROCOS_IGH_BUILD_HARDWARE_TESTS "Enable tests requiring EtherCAT hardware" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Threads REQUIRED)

add_library(rocos_igh_core INTERFACE)
target_include_directories(rocos_igh_core INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_compile_features(rocos_igh_core INTERFACE cxx_std_17)
target_link_libraries(rocos_igh_core INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(rocos_igh_core INTERFACE rt)
endif()

if(ROCOS_IGH_BUILD_MASTER)
    find_package(EtherCAT REQUIRED)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(shared_memory_config_test tests/shared_memory_config_test.cpp)
    target_link_libraries(shared_memory_config_test PRIVATE rocos_igh_core)
    add_test(NAME shared_memory_config COMMAND shared_memory_config_test)
endif()
```

- [ ] **Step 5: Configure, build, and run the smoke test without IgH development files**

Run: `cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF && cmake --build build && ctest --test-dir build --output-on-failure`

Expected: one test named `shared_memory_config` passes. If compilation reports `std::mutex` as undefined, add `#include <mutex>` to `src/shared_memory_config.hpp`; this is a missing direct include, not a new dependency.

- [ ] **Step 6: Verify missing IgH produces a clear configuration error**

Run: `cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON`

Expected without IgH development files: configuration stops with `Could NOT find EtherCAT` and identifies the missing include or library. Expected with IgH installed: configuration succeeds and records `EtherCAT_INCLUDE_DIR` and `EtherCAT_LIBRARY` in the cache.

- [ ] **Step 7: Commit the build foundation**

```bash
git add CMakeLists.txt cmake/FindEtherCAT.cmake cmake/FindEtherCAT.make tests/shared_memory_config_test.cpp src/shared_memory_config.hpp
git commit -m "build: add minimal CMake and EtherCAT discovery"
```

---

### Task 2: Safe Shared-Memory Ownership and Isolation

**Files:**
- Modify: `src/shared_memory_config.hpp`
- Modify: `tests/shared_memory_config_test.cpp`

**Interfaces:**
- Consumes: existing `SharedMemoryConfig(int)`, `createSharedMemory()`, `createPdDataMemoryProvider(int, int)`, `getSharedMemory()`, and `getPdDataMemoryProvider()`.
- Produces: `bool notifyClients() noexcept`; compatibility wrapper `void updateSempahore()`; bounded `bool waitForSignal(int) noexcept`; owner-only unlink on destruction; strict non-negative master IDs and positive PDO sizes.

- [ ] **Step 1: Add failing tests for invalid IDs and sizes**

Use a tiny local check macro so tests work in Release builds:

```cpp
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; \
        return false; \
    } \
} while (false)

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
```

Define `uniqueMasterId()` as `10000 + static_cast<int>(::getpid() % 10000)` so concurrent test processes use different names.

- [ ] **Step 2: Add a failing master/client isolation and notification test**

```cpp
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
```

- [ ] **Step 3: Run the new tests and capture the first concrete failure**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`

Expected: FAIL because negative IDs/sizes are accepted, POSIX semaphore names are not normalized consistently, `notifyClients()` does not exist, or `waitForSignal()` cannot report invalid IDs.

- [ ] **Step 4: Add ownership flags and POSIX name helpers**

Add private state without changing shared structs:

```cpp
bool owns_ecm_{false};
bool owns_pd_input_{false};
bool owns_pd_output_{false};
bool owns_semaphores_{false};

std::string semaphoreName(int index) const {
    return toPosixName(mutexName + std::to_string(index));
}
```

Reject `id < 0` in the constructor with `std::invalid_argument`. Create owner semaphores with `sem_open(name, O_CREAT | O_EXCL, 0660, 0)` after unlinking stale names; the initial value must be zero so no client observes readiness before the first publication. Set ownership only after successful creation. The destructor closes every mapping and descriptor, then calls `shm_unlink`/`sem_unlink` only for resources this instance created.

- [ ] **Step 5: Separate owner creation from client connection**

Change client methods to open existing objects only:

```cpp
sem_mutex[i] = sem_open(semaphoreName(i).c_str(), 0);
ecm_fd_ = shm_open(toPosixName(ecmName).c_str(), O_RDWR, 0);
pd_input_fd_ = shm_open(toPosixName(pdInputName).c_str(), O_RDWR, 0);
pd_output_fd_ = shm_open(toPosixName(pdOutputName).c_str(), O_RDWR, 0);
```

Do not call `O_CREAT` or `ftruncate` from client methods. Require `fstat` sizes to be positive and no larger than `EC_SHM_MAX_SIZE`. Validate owner PDO sizes with `size > 0 && size <= EC_SHM_MAX_SIZE` before unlinking or creating anything.

- [ ] **Step 6: Implement bounded wait and notification while preserving the misspelled API**

```cpp
bool notifyClients() noexcept {
    bool ok = true;
    for (sem_t *sem : sem_mutex) {
        if (sem == nullptr || sem == SEM_FAILED) {
            ok = false;
            continue;
        }
        int value = 0;
        if (sem_getvalue(sem, &value) != 0 || (value < 1 && sem_post(sem) != 0)) {
            ok = false;
        }
    }
    return ok;
}

void updateSempahore() {
    (void)notifyClients();
}

bool waitForSignal(int id = 0) noexcept {
    if (id < 0 || id >= EC_SEM_NUM || sem_mutex[id] == nullptr || sem_mutex[id] == SEM_FAILED) {
        return false;
    }
    while (sem_wait(sem_mutex[id]) != 0) {
        if (errno != EINTR) return false;
    }
    return true;
}
```

Keep `wait()` source-compatible; have it call the boolean overload and log only outside the master cyclic thread.

- [ ] **Step 7: Add a second-owner isolation test**

Create `id` and `id + 1`, write different 32-bit patterns to both `pd_input` mappings, and verify each direct client observes only its matching ID. Destroy the first owner and verify the second client remains readable.

- [ ] **Step 8: Run the IPC test repeatedly**

Run: `cmake --build build && for run in 1 2 3 4 5; do ctest --test-dir build --output-on-failure || exit 1; done`

Expected: all five runs pass without stale-object or cross-master failures.

- [ ] **Step 9: Commit the IPC lifecycle**

```bash
git add src/shared_memory_config.hpp tests/shared_memory_config_test.cpp
git commit -m "fix: make shared IPC ownership isolated and bounded"
```

---

### Task 3: Compile-Time Slave Configuration Contract

**Files:**
- Create: `src/slave_config.hpp`
- Create: `src/slave_config.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/shared_memory_config_test.cpp`

**Interfaces:**
- Consumes: IgH descriptor types from `ecrt.h` when `ROCOS_IGH_BUILD_MASTER=ON`.
- Produces: `enum class PdoDirection`; `PdoEntrySpec`; `SlaveSpec`; `StaticSlaveConfig`; `StaticSlaveConfig defaultSlaveConfig() noexcept`; `bool validateSlaveConfig(const StaticSlaveConfig &, std::string &) noexcept`.

- [ ] **Step 1: Add failing validation tests**

```cpp
bool testSlaveConfigValidation() {
    std::string error;
    CHECK(rocos::validateSlaveConfig(rocos::defaultSlaveConfig(), error));

    rocos::PdoEntrySpec invalid_entry{
        "status", rocos::PdoDirection::Input, 0x6000, 1, 7, 0, 0
    };
    rocos::SlaveSpec invalid_slave{
        0, 0, 0x00000002, 0x12345678, "invalid", nullptr,
        &invalid_entry, 1
    };
    const rocos::StaticSlaveConfig invalid_config{&invalid_slave, 1};
    CHECK(!rocos::validateSlaveConfig(invalid_config, error));
    CHECK(error.find("byte-aligned") != std::string::npos);
    return true;
}
```

- [ ] **Step 2: Run the test to verify the contract is absent**

Run: `cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON && cmake --build build-master`

Expected: compile fails because `slave_config.hpp` and its types do not exist.

- [ ] **Step 3: Define byte-oriented mutable PDO registration descriptors**

```cpp
namespace rocos {

enum class PdoDirection { Input, Output };

struct PdoEntrySpec {
    const char *name;
    PdoDirection direction;
    std::uint16_t index;
    std::uint8_t sub_index;
    std::uint8_t bit_length;
    unsigned int offset;
    unsigned int bit_position;
};

struct SlaveSpec {
    std::uint16_t alias;
    std::uint16_t position;
    std::uint32_t vendor_id;
    std::uint32_t product_code;
    const char *name;
    const ec_sync_info_t *syncs;
    PdoEntrySpec *entries;
    std::size_t entry_count;
};

struct StaticSlaveConfig {
    SlaveSpec *slaves;
    std::size_t slave_count;
};

StaticSlaveConfig defaultSlaveConfig() noexcept;
bool validateSlaveConfig(const StaticSlaveConfig &config, std::string &error) noexcept;

}  // namespace rocos
```

Offsets are mutable because IgH writes registration results during initialization. `bit_position` must be zero in the first version; reject non-byte-aligned entries because `PdVar` only stores byte offset and byte size.

- [ ] **Step 4: Implement an explicit empty default and deterministic validation**

```cpp
StaticSlaveConfig defaultSlaveConfig() noexcept {
    return {nullptr, 0};
}
```

Treat the empty default as structurally valid so unit tests and library builds do not invent hardware. For non-empty configurations require non-null slaves, non-zero vendor/product IDs, non-empty names, non-null sync tables, entry counts within `MAX_PDINPUT_NUM`/`MAX_PDOUTPUT_NUM`, non-null entry names, `bit_length > 0`, `bit_length % 8 == 0`, and `bit_position == 0`. Return the first exact failure in `error`.

- [ ] **Step 5: Convert the core target to a static library**

```cmake
add_library(rocos_igh_core STATIC
    src/slave_config.cpp
)
target_include_directories(rocos_igh_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_compile_features(rocos_igh_core PUBLIC cxx_std_17)
target_link_libraries(rocos_igh_core PUBLIC
    EtherCAT::EtherCAT
    Threads::Threads
)
```

Move `find_package(EtherCAT REQUIRED)` ahead of this target when the master build is enabled. When `ROCOS_IGH_BUILD_MASTER=OFF`, retain an interface-only IPC target named `rocos_igh_core` and compile only the IPC test sections guarded from IgH-specific headers.

- [ ] **Step 6: Run configuration tests**

Run: `cmake --build build-master && ctest --test-dir build-master --output-on-failure`

Expected: empty config passes structural validation; the 7-bit entry fails with the exact byte-alignment message.

- [ ] **Step 7: Commit the static configuration contract**

```bash
git add CMakeLists.txt src/slave_config.hpp src/slave_config.cpp tests/shared_memory_config_test.cpp
git commit -m "feat: define compile-time PDO configuration contract"
```

---

### Task 4: IgH Master Lifecycle and Two Domains

**Files:**
- Create: `src/ethercat_master.hpp`
- Create: `src/ethercat_master.cpp`
- Modify: `src/slave_config.hpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/shared_memory_config_test.cpp`

**Interfaces:**
- Consumes: validated `StaticSlaveConfig`; `ecrt_request_master`, domain/configuration/registration APIs, `ecrt_master_activate`, `ecrt_domain_data`, and state APIs.
- Produces: move-disabled RAII class `EthercatMaster`; `bool initialize(unsigned int, StaticSlaveConfig, std::string &)`; `void receiveAndProcess() noexcept`; `void queueAndSend() noexcept`; input/output data and size accessors; `BusState readState() noexcept`.

- [ ] **Step 1: Add a compile-time API test**

```cpp
static_assert(!std::is_copy_constructible<rocos::EthercatMaster>::value,
              "EthercatMaster must own one master exclusively");
static_assert(!std::is_move_constructible<rocos::EthercatMaster>::value,
              "domain pointers must not outlive their owner");
```

- [ ] **Step 2: Build to verify the class is absent**

Run: `cmake --build build-master`

Expected: compile fails because `ethercat_master.hpp` does not exist.

- [ ] **Step 3: Define the narrow lifecycle API**

```cpp
struct BusState {
    unsigned int responding_slaves{0};
    unsigned int al_states{0};
    bool link_up{false};
    unsigned int input_working_counter{0};
    unsigned int output_working_counter{0};
    ec_wc_state_t input_wc_state{EC_WC_ZERO};
    ec_wc_state_t output_wc_state{EC_WC_ZERO};
};

class EthercatMaster {
public:
    EthercatMaster() = default;
    ~EthercatMaster();
    EthercatMaster(const EthercatMaster &) = delete;
    EthercatMaster &operator=(const EthercatMaster &) = delete;
    EthercatMaster(EthercatMaster &&) = delete;
    EthercatMaster &operator=(EthercatMaster &&) = delete;

    bool initialize(unsigned int master_id, StaticSlaveConfig config,
                    std::string &error);
    void receiveAndProcess() noexcept;
    void queueAndSend() noexcept;
    BusState readState() noexcept;

    std::uint8_t *inputData() noexcept;
    std::uint8_t *outputData() noexcept;
    std::size_t inputSize() const noexcept;
    std::size_t outputSize() const noexcept;
    bool initialized() const noexcept;
};
```

- [ ] **Step 4: Implement fail-fast initialization in IgH phase order**

Implement this exact ownership sequence:

1. Reject an already initialized object and an empty `StaticSlaveConfig` with `error = "no slave configuration compiled"`.
2. Validate the static configuration.
3. Call `ecrt_request_master(master_id)`.
4. Create one input and one output domain.
5. For every `SlaveSpec`, call `ecrt_master_slave_config` and `ecrt_slave_config_pdos(sc, EC_END, syncs)`.
6. Register each input entry against the input domain and each output entry against the output domain using `ecrt_slave_config_reg_pdo_entry`; save byte offset and bit position back to `PdoEntrySpec`.
7. Reject non-zero returned bit positions.
8. Call `ecrt_master_activate` once.
9. Cache both `ecrt_domain_data` pointers and `ecrt_domain_size` values; reject null pointers, zero lengths, or lengths above `EC_SHM_MAX_SIZE`.

On every failure, set a specific non-real-time error string and call one private `reset() noexcept` that invokes `ecrt_release_master` at most once and nulls every borrowed pointer.

- [ ] **Step 5: Implement the fixed cyclic methods**

```cpp
void EthercatMaster::receiveAndProcess() noexcept {
    ecrt_master_receive(master_);
    ecrt_domain_process(input_domain_);
    ecrt_domain_process(output_domain_);
}

void EthercatMaster::queueAndSend() noexcept {
    ecrt_domain_queue(input_domain_);
    ecrt_domain_queue(output_domain_);
    ecrt_master_send(master_);
}
```

`readState()` calls `ecrt_master_state` and `ecrt_domain_state` for both domains and returns a value object without logging or allocation.

- [ ] **Step 6: Add metadata publication**

Add `bool publishConfig(EcatBus &, const StaticSlaveConfig &, std::string &)` beside the static config implementation. It clears `EcatBus`, bounds-checks slave and per-direction counts, copies names with guaranteed null termination, and writes each entry's byte `offset`, `size = bit_length / 8`, `index`, and `sub_index` into the matching `PdVar`.

Test one synthetic slave with one 16-bit input and one 32-bit output and assert every published field exactly.

- [ ] **Step 7: Build and run hardware-free tests**

Run: `cmake --build build-master && ctest --test-dir build-master --output-on-failure`

Expected: compile-time ownership checks and metadata publication tests pass. No test calls `ecrt_request_master` in the default suite.

- [ ] **Step 8: Commit the IgH lifecycle**

```bash
git add CMakeLists.txt src/ethercat_master.hpp src/ethercat_master.cpp src/slave_config.hpp src/slave_config.cpp tests/shared_memory_config_test.cpp
git commit -m "feat: add two-domain IgH master lifecycle"
```

---

### Task 5: Deterministic Cyclic Task

**Files:**
- Create: `src/cyclic_task.hpp`
- Create: `src/cyclic_task.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/shared_memory_config_test.cpp`

**Interfaces:**
- Consumes: initialized `EthercatMaster`, mapped `SharedMemoryConfig`, period in microseconds, and `volatile std::sig_atomic_t` stop flag.
- Produces: `CycleStatistics`; pure `copyProcessData(...) noexcept`; pure `advanceDeadline(...) noexcept`; pure `updateSharedBus(...) noexcept`; `CyclicTask::run(...) noexcept`.

- [ ] **Step 1: Add failing fixed-copy tests**

```cpp
bool testProcessDataCopy() {
    std::uint8_t domain_input[4]{1, 2, 3, 4};
    std::uint8_t shared_input[4]{};
    std::uint8_t shared_output[4]{5, 6, 7, 8};
    std::uint8_t domain_output[4]{};

    rocos::copyProcessData(domain_input, sizeof(domain_input),
                           shared_input, shared_output,
                           domain_output, sizeof(domain_output));

    CHECK(std::memcmp(domain_input, shared_input, 4) == 0);
    CHECK(std::memcmp(shared_output, domain_output, 4) == 0);
    return true;
}
```

- [ ] **Step 2: Add a failing missed-deadline test**

```cpp
bool testDeadlineAdvanceSkipsCatchUp() {
    const timespec previous{10, 0};
    const timespec now{10, 3'500'000};
    std::uint64_t missed = 0;
    const timespec next = rocos::advanceDeadline(previous, 1000, now, missed);
    CHECK(next.tv_sec == 10);
    CHECK(next.tv_nsec == 4'000'000);
    CHECK(missed == 3);
    return true;
}
```

- [ ] **Step 3: Build to verify cyclic helpers are absent**

Run: `cmake --build build-master`

Expected: compile fails because `copyProcessData` and `advanceDeadline` are undefined.

- [ ] **Step 4: Implement allocation-free helpers**

```cpp
void copyProcessData(const std::uint8_t *domain_input,
                     std::size_t input_size,
                     void *shared_input,
                     const void *shared_output,
                     std::uint8_t *domain_output,
                     std::size_t output_size) noexcept {
    std::memcpy(shared_input, domain_input, input_size);
    std::memcpy(domain_output, shared_output, output_size);
}
```

Implement `advanceDeadline` with normalized integer nanoseconds. Add `period_us * 1000` repeatedly in arithmetic, calculate skipped intervals directly rather than looping, and return the first deadline strictly greater than `now`.

- [ ] **Step 5: Define statistics and the task API**

```cpp
struct CycleStatistics {
    std::uint64_t cycles{0};
    std::uint64_t missed_deadlines{0};
    double minimum_us{0.0};
    double maximum_us{0.0};
    double average_us{0.0};
    double current_us{0.0};
};

class CyclicTask {
public:
    CyclicTask(EthercatMaster &master, SharedMemoryConfig &ipc,
               std::uint32_t period_us) noexcept;
    int run(volatile std::sig_atomic_t &stop_requested) noexcept;
    const CycleStatistics &statistics() const noexcept;
};
```

Reject periods below `1000 us` before constructing this object.

Define the shared-state updater explicitly:

```cpp
void updateSharedBus(const BusState &state,
                     CycleStatistics &statistics,
                     std::uint32_t period_us,
                     long monotonic_timestamp_us,
                     EcatBus &bus) noexcept;
```

If `bus.resetCycleTime` is set, clear all timing aggregates before accounting for the current cycle and reset the flag to `false`. Then write `timestamp` as `CLOCK_MONOTONIC` microseconds, `dt = period_us`, all four cycle-time fields from `CycleStatistics`, and `current_state = static_cast<int>(state.al_states)`. Set `is_authorized` only when the link is up, `responding_slaves == bus.slave_num`, and both domain WC states equal `EC_WC_COMPLETE`. Leave client-owned `request_state` and internal `next_expected_state` unchanged because the first version has no runtime state-request protocol.

- [ ] **Step 6: Implement the cyclic order exactly once**

Inside `run`, use `clock_gettime(CLOCK_MONOTONIC)` and `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)`, retrying only on `EINTR`. Each successful wake performs:

```cpp
master_.receiveAndProcess();
copyProcessData(master_.inputData(), master_.inputSize(), ipc_.pdInputPtr,
                ipc_.pdOutputPtr, master_.outputData(), master_.outputSize());
const BusState state = master_.readState();
updateSharedBus(state, statistics_, *ipc_.ecatBus);
master_.queueAndSend();
ipc_.notifyClients();
```

`updateSharedBus` writes only existing `EcatBus` fields. Keep WKC and deadline counters in `CycleStatistics`; do not extend the shared ABI. When a deadline is missed, advance directly to the nearest future deadline. Return a non-zero errno-style value only for clock failures.

- [ ] **Step 7: Run helper and IPC tests**

Run: `cmake --build build-master && ctest --test-dir build-master --output-on-failure`

Expected: copy direction is exact, deadline test returns 4 ms with three missed intervals, and all IPC tests remain green.

- [ ] **Step 8: Commit the cyclic core**

```bash
git add CMakeLists.txt src/cyclic_task.hpp src/cyclic_task.cpp tests/shared_memory_config_test.cpp
git commit -m "feat: add allocation-free cyclic PDO task"
```

---

### Task 6: Process Entry, Real-Time Setup, and Final Validation

**Files:**
- Create: `src/runtime_options.hpp`
- Create: `src/runtime_options.cpp`
- Create: `src/main.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/shared_memory_config_test.cpp`
- Modify: `README.md`

**Interfaces:**
- Consumes: `defaultSlaveConfig`, `EthercatMaster`, `SharedMemoryConfig`, and `CyclicTask`.
- Produces: `RuntimeOptions`; `bool parseRuntimeOptions(int, char **, RuntimeOptions &, std::string &)`; executable `rocos_igh_master`; documented startup and test commands.

- [ ] **Step 1: Add failing option parser tests**

```cpp
bool testRuntimeOptions() {
    char program[] = "rocos_igh_master";
    char master_flag[] = "--master-id";
    char master_value[] = "1";
    char period_flag[] = "--period-us";
    char period_value[] = "1000";
    char *valid[]{program, master_flag, master_value, period_flag, period_value};

    rocos::RuntimeOptions options{};
    std::string error;
    CHECK(rocos::parseRuntimeOptions(5, valid, options, error));
    CHECK(options.master_id == 1U);
    CHECK(options.period_us == 1000U);

    char short_period[] = "999";
    char *invalid[]{program, period_flag, short_period};
    CHECK(!rocos::parseRuntimeOptions(3, invalid, options, error));
    return true;
}
```

- [ ] **Step 2: Build to verify the parser is absent**

Run: `cmake --build build-master`

Expected: compile fails because `runtime_options.hpp` does not exist.

- [ ] **Step 3: Implement strict options without external libraries**

```cpp
struct RuntimeOptions {
    unsigned int master_id{0};
    std::uint32_t period_us{1000};
};
```

Accept only `--master-id <non-negative integer>`, `--period-us <integer >= 1000>`, and `--help`. Reject unknown flags, missing values, signs, trailing characters, and integer overflow using `std::from_chars`.

- [ ] **Step 4: Implement real-time setup before the cycle starts**

Add private functions in `main.cpp`:

```cpp
bool configureRealtime(std::string &error);
void handleSignal(int) noexcept;
```

`configureRealtime` calls `mlockall(MCL_CURRENT | MCL_FUTURE)`, pre-touches a fixed 64 KiB stack array, and applies `SCHED_FIFO` with a named constant priority of 80. Failure is fatal and reported before entering the cyclic task. The signal handler only assigns `1` to a global `volatile std::sig_atomic_t`.

- [ ] **Step 5: Assemble startup in the documented order**

`main` performs these steps and no others:

1. Parse options and install `SIGINT`/`SIGTERM` handlers with `sigaction`.
2. Obtain `StaticSlaveConfig config = defaultSlaveConfig()` and reject `config.slave_count == 0` with `no slave configuration compiled`.
3. Initialize `EthercatMaster`.
4. Construct `SharedMemoryConfig(options.master_id)` and create bus IPC.
5. Create PDO IPC with the two domain sizes.
6. Call `publishConfig`, set `ecatBus->dt`, and initialize state fields.
7. Configure real-time memory and scheduling.
8. Construct and run `CyclicTask` until the signal flag changes.
9. Print one final non-real-time statistics line after `run` returns; destruct objects in reverse order.

- [ ] **Step 6: Add the executable target**

```cmake
if(ROCOS_IGH_BUILD_MASTER)
    target_sources(rocos_igh_core PRIVATE
        src/ethercat_master.cpp
        src/cyclic_task.cpp
        src/runtime_options.cpp
    )
    add_executable(rocos_igh_master src/main.cpp)
    target_link_libraries(rocos_igh_master PRIVATE rocos_igh_core)
endif()
```

- [ ] **Step 7: Update the README with current truth**

Document both build modes:

```bash
# IPC tests without IgH development files
cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF
cmake --build build
ctest --test-dir build --output-on-failure

# Full master build with ecrt.h and libethercat installed
cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON
cmake --build build-master
ctest --test-dir build-master --output-on-failure
```

State explicitly that the checked-in default slave table is empty, so `rocos_igh_master` intentionally exits until a device-specific static PDO table is added. Link to `docs/architecture.md`, `docs/api-guide.md`, and `docs/ethercat-terminal-commands.md` rather than duplicating them.

- [ ] **Step 8: Run complete hardware-free validation**

Run: `cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF && cmake --build build && ctest --test-dir build --output-on-failure`

Expected: all IPC-only tests pass with zero failures.

Run when IgH development files are installed: `cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON && cmake --build build-master && ctest --test-dir build-master --output-on-failure`

Expected: `rocos_igh_core`, `rocos_igh_master`, and `shared_memory_config_test` build; all default tests pass without opening `/dev/EtherCAT*`.

- [ ] **Step 9: Verify the empty hardware configuration fails safely**

Run: `./build-master/rocos_igh_master --master-id 0 --period-us 1000`

Expected: exits non-zero with exactly `no slave configuration compiled`; it does not request a master, create shared memory, or require real-time privileges.

- [ ] **Step 10: Inspect diagnostics and changed files**

Run: `git diff --check && git status --short`

Expected: no whitespace errors; changes are limited to the files listed in this plan plus the approved design and plan documents.

- [ ] **Step 11: Commit the runnable process shell**

```bash
git add CMakeLists.txt README.md src/main.cpp src/runtime_options.hpp src/runtime_options.cpp tests/shared_memory_config_test.cpp
git commit -m "feat: add minimal IgH master process entry"
```

## Deferred Hardware Task

A device-specific task begins only after the user supplies the target slave model, vendor ID, product code, Sync Manager/PDO table, and PDO entry index/subindex/bit length. That task adds one static table in `src/slave_config.cpp`, validates it with a metadata unit test, enables the opt-in hardware test, and verifies state transitions, WKC, real PDO exchange, simultaneous master 0/1 isolation, and 1 ms timing on the target system.