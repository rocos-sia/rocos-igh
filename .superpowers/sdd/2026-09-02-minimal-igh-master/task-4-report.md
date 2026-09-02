# Task 4 Report - IgH Master Lifecycle and Two Domains

## Pre-edit research and scope confirmation

- Source scope confirmed from task brief: create `src/ethercat_master.hpp`, `src/ethercat_master.cpp`; modify `src/slave_config.hpp`, `src/slave_config.cpp`, `CMakeLists.txt`, `tests/shared_memory_config_test.cpp`.
- Existing shared-memory ABI structures (`EcatBus`, `Slave`, `PdVar`) are header-defined in `src/shared_memory_config.hpp`; field order and limits must remain unchanged.
- Build modes confirmed:
  - Master-enabled: `ROCOS_IGH_BUILD_MASTER=ON` links IgH via `find_package(EtherCAT REQUIRED)`.
  - Hardware-free default tests must avoid requesting a master (`ecrt_request_master`) in default suite.
- Existing tests are in `tests/shared_memory_config_test.cpp`; this file already conditionally compiles master-only tests under `#if ROCOS_IGH_BUILD_MASTER`.
- User constraint confirmed: do not modify `cmake/FindEtherCAT.make` (pre-existing user change).

## Decomposition gate verdict

- Scenario skill root for execution-stage decomposition hints was not forwarded in this dispatch context.
- Based on repository scope and task shape, this task is treated as **atomic**: one cohesive feature introducing one RAII owner class + metadata publishing + associated tests.

## TDD execution plan

1. Add compile-time API ownership test that includes `ethercat_master.hpp` and asserts non-copyable/non-movable.
2. Build `build-master` to confirm RED phase failure because header/class is absent.
3. Add metadata publication test using synthetic static slave config and exact `EcatBus` field assertions.
4. Implement `publishConfig(...)` API and logic in slave config module.
5. Implement `EthercatMaster` API in new files with phase-ordered initialization, reset semantics, cyclic methods, and state snapshots.
6. Wire new source files into CMake master build target.
7. Run full validation:
   - Master build + CTest
   - Hardware-free build + CTest
8. Self-review against task brief checklist and record outcomes, risks, and evidence here.

## Execution log (TDD)

1. RED:
   - Added compile-time ownership checks in `tests/shared_memory_config_test.cpp` that include `ethercat_master.hpp` and `static_assert` non-copyable/non-movable semantics.
   - Ran `cmake --build build-master` and confirmed expected failure:
     - `fatal error: ethercat_master.hpp: No such file or directory`.

2. RED extension for metadata behavior:
   - Added `testPublishConfigMetadata()` to assert exact `EcatBus` publication for one synthetic slave with one 16-bit input and one 32-bit output entry.

3. GREEN implementation:
   - Created `src/ethercat_master.hpp` and `src/ethercat_master.cpp`.
   - Added strict ownership API and lifecycle methods:
     - Deleted copy/move ctor and assignment.
     - `initialize(...)` performs phase-ordered setup and fail-fast reset.
     - `receiveAndProcess()` and `queueAndSend()` use fixed cyclic order.
     - `readState()` returns state snapshot object without logging/allocation.
   - Added `publishConfig(EcatBus &, const StaticSlaveConfig &, std::string &)` declaration and implementation in slave-config module.
   - Updated `CMakeLists.txt` to compile `src/ethercat_master.cpp` into the master-enabled core library.

4. Final compatibility adjustment:
   - Guarded non-master fallback type declaration in `src/slave_config.hpp` with `#ifndef __ECRT_H__` so mixed include order does not redefine `ec_sync_info_t` in editor/indexer contexts.

## Requirement-by-requirement self-review

- Exclusive RAII ownership of one master:
  - `EthercatMaster` is non-copyable and non-movable; destructor calls `reset()`.
- Two domains:
  - `initialize(...)` creates and tracks one input domain and one output domain.
- Phase-ordered initialization:
  - Order implemented as required: validate -> request master -> create domains -> slave config/pdos -> per-entry registration -> activate -> cache data pointers/sizes.
- Fail-fast and cleanup:
  - Every failure path sets a specific non-RT error string and calls single `reset() noexcept` that releases master at most once and nulls borrowed pointers.
- Cyclic operations:
  - `receiveAndProcess()` uses `receive -> process input -> process output`.
  - `queueAndSend()` uses `queue input -> queue output -> send`.
- State snapshots:
  - `readState()` collects `ec_master_state_t` and both `ec_domain_state_t` values into `BusState`.
- EcatBus metadata publication:
  - `publishConfig(...)` clears `EcatBus`, bounds-checks counts/ranges, copies names with null termination, publishes offset/size/index/sub-index into `PdVar`.
- Preserve SharedMemoryConfig ABI:
  - No ABI layout changes were made in `src/shared_memory_config.hpp`.
- Default tests hardware-free:
  - No default (master-off) test path calls `ecrt_request_master`; master-specific checks remain under `#if ROCOS_IGH_BUILD_MASTER`.
- Do not touch user-modified finder file:
  - `cmake/FindEtherCAT.make` was not edited.

## Validation evidence

- Master-enabled validation:
  - `cmake --build build-master`
  - `ctest --test-dir build-master --output-on-failure`
  - Result: `1/1` passed, `0` failed.

- Hardware-free validation:
  - `cmake -S . -B build-nomaster -DROCOS_IGH_BUILD_MASTER=OFF -DROCOS_IGH_BUILD_HARDWARE_TESTS=OFF`
  - `cmake --build build-nomaster`
  - `ctest --test-dir build-nomaster --output-on-failure`
  - Result: `1/1` passed, `0` failed.

## Notes and residual concerns

- `ecrt_slave_config_reg_pdo_entry(...)` signature in installed IgH returns byte offset directly and outputs bit position via pointer; implementation follows this API and stores both results in `PdoEntrySpec`.
- Runtime behavior of `initialize(...)` failure paths that require real hardware (for example, missing slave at scan time) is not covered by default tests and remains for hardware-marked integration coverage.

## Commits

- `2e52f5d` feat: add two-domain IgH master lifecycle
- `a2d7f83` test: add ownership and metadata publication coverage

## Post-commit verification

- Re-ran both required paths after final commits:
  - `cmake --build build-master && ctest --test-dir build-master --output-on-failure`
  - `cmake --build build-nomaster && ctest --test-dir build-nomaster --output-on-failure`
- Result: both suites passed (`1/1` each, `0` failures).

## Fix Round 1 (Review Findings)

### Findings addressed

1. BLOCKING fixed: `EthercatMaster::initialize()` now routes all early-failure exits (`already initialized`, `empty config`, `validateSlaveConfig` failure) through `reset()` before returning `false`.
2. BLOCKING fixed: `receiveAndProcess()` and `queueAndSend()` now have a fast `noexcept` pre-init/null-pointer guard and return immediately unless initialization completed and all required pointers are valid.
3. WARNING fixed: `publishConfig()` now rejects entries where `offset + byte_size` exceeds `EC_SHM_MAX_SIZE` using overflow-safe arithmetic (`byte_size > EC_SHM_MAX_SIZE - offset`) after validating `offset <= EC_SHM_MAX_SIZE`.

### Added focused tests

- `testMasterCyclicCallsBeforeInitialization()` verifies cyclic methods are callable before init without dereferencing null pointers and that default state remains zeroed.
- `testPublishConfigMetadataBounds()` verifies:
  - valid boundary case (`offset=EC_SHM_MAX_SIZE-2`, `size=2`) succeeds;
  - overflow case (`offset=EC_SHM_MAX_SIZE-2`, `size=4`) is rejected with the expected bounds error.

### Verification commands and outputs

1. `cmake --build build-master`

```text
[ 20%] Building CXX object CMakeFiles/rocos_igh_core.dir/src/slave_config.cpp.o
[ 40%] Building CXX object CMakeFiles/rocos_igh_core.dir/src/ethercat_master.cpp.o
[ 60%] Linking CXX static library librocos_igh_core.a
[ 60%] Built target rocos_igh_core
[ 80%] Building CXX object CMakeFiles/shared_memory_config_test.dir/tests/shared_memory_config_test.cpp.o
[100%] Linking CXX executable shared_memory_config_test
[100%] Built target shared_memory_config_test
```

2. `ctest --test-dir build-master --output-on-failure`

```text
Internal ctest changing into directory: /home/think/Documents/GitHub/rocos-igh/.worktrees/minimal-igh-master/build-master
Test project /home/think/Documents/GitHub/rocos-igh/.worktrees/minimal-igh-master/build-master
    Start 1: shared_memory_config
1/1 Test #1: shared_memory_config .............   Passed    0.01 sec

100% tests passed, 0 tests failed out of 1

Total Test time (real) =   0.01 sec
```

3. `cmake --build build-nomaster`

```text
[ 50%] Building CXX object CMakeFiles/shared_memory_config_test.dir/tests/shared_memory_config_test.cpp.o
[100%] Linking CXX executable shared_memory_config_test
[100%] Built target shared_memory_config_test
```

4. `ctest --test-dir build-nomaster --output-on-failure`

```text
Internal ctest changing into directory: /home/think/Documents/GitHub/rocos-igh/.worktrees/minimal-igh-master/build-nomaster
Test project /home/think/Documents/GitHub/rocos-igh/.worktrees/minimal-igh-master/build-nomaster
    Start 1: shared_memory_config
1/1 Test #1: shared_memory_config .............   Passed    0.01 sec

100% tests passed, 0 tests failed out of 1

Total Test time (real) =   0.01 sec
```

## Fix Round 2 (Human override: preserve live master on repeated initialize)

### Important finding addressed

- Updated `EthercatMaster::initialize()` so `initialized_` precondition failure returns `false` with `"master already initialized"` **without** calling `reset()`.
- All fresh/partial initialization failure paths (`empty config`, config validation failure, request/create/register/activate/data-size failures) continue to call `reset()`.

### Added hardware-free regression coverage

- Added a test-only friend peer (`ROCOS_IGH_TESTING`) to seed private state in `EthercatMaster` without invoking IgH/hardware.
- Added `testInitializeRejectsAlreadyInitializedWithoutReset()`:
  - seeds an initialized object with sentinel input/output pointers and sizes,
  - calls `initialize(...)`,
  - asserts failure + exact error string,
  - asserts seeded state is preserved (no reset side effects).
- Added `testInitializeFreshFailureResetsState()`:
  - seeds a non-initialized object with sentinel state,
  - calls `initialize(...)` with empty config,
  - asserts failure + exact error string,
  - asserts state is cleared by reset.

### Hardware-free test limitations

- Without hardware or link-time symbol interception/mocking of `ecrt_release_master`, tests cannot directly prove that no release call occurred on the already-initialized branch.
- The strongest practical hardware-free contract proof here is observable state preservation (no reset side effects) plus preserved reset behavior on fresh/partial failures.

### Verification evidence (exact)

1. `cmake --build build-master && ctest --test-dir build-master --output-on-failure`

```text
[ 60%] Built target rocos_igh_core
Consolidate compiler generated dependencies of target shared_memory_config_test
[ 80%] Building CXX object CMakeFiles/shared_memory_config_test.dir/tests/shared_memory_config_test.cpp.o
[100%] Linking CXX executable shared_memory_config_test
[100%] Built target shared_memory_config_test
Internal ctest changing into directory: /home/think/Documents/GitHub/rocos-igh/.worktrees/minimal-igh-master/build-master
Test project /home/think/Documents/GitHub/rocos-igh/.worktrees/minimal-igh-master/build-master
    Start 1: shared_memory_config
1/1 Test #1: shared_memory_config .............   Passed    0.01 sec

100% tests passed, 0 tests failed out of 1

Total Test time (real) =   0.01 sec
```

2. `cmake -S . -B build-nomaster -DROCOS_IGH_BUILD_MASTER=OFF -DROCOS_IGH_BUILD_HARDWARE_TESTS=OFF && cmake --build build-nomaster && ctest --test-dir build-nomaster --output-on-failure`

```text
-- Configuring done
-- Generating done
-- Build files have been written to: /home/think/Documents/GitHub/rocos-igh/.worktrees/minimal-igh-master/build-nomaster
Consolidate compiler generated dependencies of target shared_memory_config_test
[ 50%] Building CXX object CMakeFiles/shared_memory_config_test.dir/tests/shared_memory_config_test.cpp.o
[100%] Linking CXX executable shared_memory_config_test
[100%] Built target shared_memory_config_test
Internal ctest changing into directory: /home/think/Documents/GitHub/rocos-igh/.worktrees/minimal-igh-master/build-nomaster
Test project /home/think/Documents/GitHub/rocos-igh/.worktrees/minimal-igh-master/build-nomaster
    Start 1: shared_memory_config
1/1 Test #1: shared_memory_config .............   Passed    0.01 sec

100% tests passed, 0 tests failed out of 1

Total Test time (real) =   0.01 sec
```
