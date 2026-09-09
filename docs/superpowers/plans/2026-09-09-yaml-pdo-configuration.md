# YAML PDO Configuration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load each slave's PDO mapping from a required startup YAML file, convert it to IgH configuration structures, and print the resolved mapping before cyclic operation.

**Architecture:** A hardware-independent `PdoBusConfig` model and yaml-cpp parser provide validation in both build modes. In master mode, an owning `LoadedSlaveConfig` converts that model into stable IgH arrays and the existing `StaticSlaveConfig` pointer view; it remains alive through initialization and shared-memory publication.

**Tech Stack:** C++17, yaml-cpp 0.7-compatible API, IgH EtherCAT Master API, CMake, CTest.

## Global Constraints

- `--config <path>` is required unless `--help` is present.
- Slave IDs start at zero, are contiguous, match YAML sequence order, and become physical positions; alias is always zero.
- Vendor ID and product code are always read from SII and are not YAML fields.
- RxPDO uses SM2, `EC_DIR_OUTPUT`, and `EC_WD_ENABLE`; TxPDO uses SM3, `EC_DIR_INPUT`, and `EC_WD_DISABLE`.
- YAML supports multiple slaves and multiple PDOs in each direction.
- PDO entry widths are non-zero multiples of eight and must fit existing shared-memory limits.
- YAML parsing, allocation, SII access, and formatted output stay outside the cyclic path.
- Do not change the layout of `EcatBus`, `Slave`, `PdVar`, `EcatConfigMaster`, or `EcatConfig`.

---

### Task 1: Required Configuration CLI Option

**Files:**
- Modify: `src/runtime_options.hpp`
- Modify: `src/runtime_options.cpp`
- Modify: `tests/shared_memory_config_test.cpp`

**Interfaces:**
- Produces: `RuntimeOptions::config_path` as `std::string`.
- Produces: `parseRuntimeOptions(...)` accepting `--config <path>` and rejecting its absence unless help was requested.

- [ ] **Step 1: Write failing command-line tests**

Extend `testRuntimeOptions()` with a valid argument vector containing:

```cpp
char config_flag[] = "--config";
char config_value[] = "config/pdo.yaml";
char *valid[]{program, master_flag, master_value, period_flag, period_value,
              config_flag, config_value};
```

Assert `options.config_path == "config/pdo.yaml"`. Add separate assertions that normal arguments without `--config`, an empty config value, and duplicate `--config` fail, while `--help` without a config succeeds.

- [ ] **Step 2: Verify the CLI tests fail**

Run:

```bash
cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF
cmake --build build
ctest --test-dir build -R shared_memory_config --output-on-failure
```

Expected: compilation fails because `RuntimeOptions::config_path` does not exist, proving the new interface is under test.

- [ ] **Step 3: Implement the minimal parser change**

Add:

```cpp
std::string config_path;
```

Recognize `--config`, require a following non-empty value that does not begin with `--`, reject duplicate occurrences, and after parsing return `"missing required option: --config"` when `show_help` is false and no path was supplied. Update usage to:

```text
usage: rocos_igh_master --config <path> [--master-id <id>] [--period-us <period>] [--help]
```

- [ ] **Step 4: Verify the CLI tests pass**

Run the same configure, build, and focused CTest command. Expected: PASS.

### Task 2: Hardware-Independent YAML Parser

**Files:**
- Create: `src/pdo_config.hpp`
- Create: `src/pdo_config.cpp`
- Create: `tests/pdo_config_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `PdoEntryConfig { std::string name; uint16_t index; uint8_t sub_index; uint8_t bit_length; }`.
- Produces: `PdoMappingConfig { uint16_t index; std::vector<PdoEntryConfig> entries; }`.
- Produces: `SlavePdoConfig { uint16_t id; std::string name; std::vector<PdoMappingConfig> rx_pdos; std::vector<PdoMappingConfig> tx_pdos; }`.
- Produces: `PdoBusConfig { std::vector<SlavePdoConfig> slaves; }`.
- Produces: `bool loadPdoConfig(const std::string &path, PdoBusConfig &config, std::string &error) noexcept`.

- [ ] **Step 1: Add a failing valid-file parser test**

Create a small standalone test executable using the repository's `CHECK` style. Write a temporary YAML file containing two slaves, two RxPDOs on one slave, hexadecimal indexes, and decimal sub-indexes. Assert exact slave order, names, PDO indexes, entry indexes, directions by container, and widths.

- [ ] **Step 2: Wire yaml-cpp and verify the parser test fails**

In CMake use:

```cmake
find_package(yaml-cpp REQUIRED)
add_executable(pdo_config_test tests/pdo_config_test.cpp src/pdo_config.cpp)
target_link_libraries(pdo_config_test PRIVATE yaml-cpp)
target_compile_features(pdo_config_test PRIVATE cxx_std_17)
add_test(NAME pdo_config COMMAND pdo_config_test)
```

Run `cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF && cmake --build build`. Expected: compilation fails because the parser model and function are not implemented.

- [ ] **Step 3: Implement valid YAML parsing**

Use `YAML::LoadFile` and explicit node checks. Parse only the documented keys, require `version: 1`, require non-empty `slaves`, `rx_pdos`, `tx_pdos`, and `entries`, and build into a local `PdoBusConfig` before moving it into the output so failures do not leave partial state.

- [ ] **Step 4: Verify the valid-file test passes**

Run `ctest --test-dir build -R pdo_config --output-on-failure`. Expected: PASS.

- [ ] **Step 5: Add failing validation tests**

Add table-driven temporary-file cases for unsupported version, missing keys, unknown keys, ID/order mismatch, empty names, integer overflow, zero width, width not divisible by eight, empty PDO lists, empty entry lists, and more than `MAX_PDINPUT_NUM`/`MAX_PDOUTPUT_NUM` entries per slave. Assert each error contains a precise field path.

- [ ] **Step 6: Implement strict validation and rerun tests**

Reject unknown mapping keys so misspellings cannot silently change a machine configuration. Convert YAML numeric exceptions into errors such as:

```text
<path>: slaves[1].tx_pdos[0].entries[2].bit_length must be an unsigned 8-bit integer
```

Include `shared_memory_config.hpp` only in the implementation for existing entry-count limits. Run focused CTest and expect PASS.

### Task 3: Stable IgH Configuration Ownership And Startup Output

**Files:**
- Modify: `src/slave_config.hpp`
- Modify: `src/slave_config.cpp`
- Modify: `src/ethercat_master.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/shared_memory_config_test.cpp`

**Interfaces:**
- Consumes: `PdoBusConfig` from Task 2.
- Produces: move-only `LoadedSlaveConfig` owning all names, IgH PDO arrays, Sync Manager arrays, flattened `PdoEntrySpec` arrays, and `SlaveSpec` views.
- Produces: `bool LoadedSlaveConfig::build(PdoBusConfig config, std::string &error) noexcept`.
- Produces: `StaticSlaveConfig LoadedSlaveConfig::view() noexcept`.
- Produces: `void printLoadedSlaveConfig(const LoadedSlaveConfig &config, std::ostream &output)`.

- [ ] **Step 1: Replace static-table expectations with failing runtime-build tests**

Remove tests for `defaultSlaveConfig()`. Construct a `PdoBusConfig` with multiple PDOs and assert after `build()` that:

```cpp
const rocos::StaticSlaveConfig view = loaded.view();
CHECK(view.slave_count == 2U);
CHECK(view.slaves[0].alias == 0U);
CHECK(view.slaves[0].position == 0U);
CHECK(view.slaves[0].vendor_id == 0U);
CHECK(view.slaves[0].product_code == 0U);
CHECK(view.slaves[0].entries[0].direction == rocos::PdoDirection::Output);
```

Also assert the SM2/SM3 indexes, directions, watchdog modes, PDO indexes, entry counts, and `0xff` terminator. Build only this test in master mode and verify it fails because `LoadedSlaveConfig` is absent.

- [ ] **Step 2: Implement owned conversion with stable pointers**

For each slave, finalize all strings and vectors before assigning any `c_str()` or `.data()` pointers. Flatten Rx entries first as `PdoDirection::Output`, then Tx entries as `PdoDirection::Input`. Build exactly three Sync Manager records: SM2, SM3, sentinel. Delete the compile-time drive arrays and `defaultSlaveConfig()`.

- [ ] **Step 3: Run the focused master-mode test**

Run:

```bash
cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON
cmake --build build-master
ctest --test-dir build-master -R shared_memory_config --output-on-failure
```

Expected: PASS without accessing EtherCAT hardware.

- [ ] **Step 4: Add a failing formatted-output test**

Seed discovered identity values and domain offsets in the built view, stream output into `std::ostringstream`, and assert it contains slave ID/name, eight-digit vendor/product hex values, RxPDO/TxPDO indexes, SM2/SM3, object index/sub-index, bit width, offset, and entry name.

- [ ] **Step 5: Implement formatting and verify it passes**

Implement `printLoadedSlaveConfig` without hardware calls. Preserve stream flags/fill after formatting. Run the focused master-mode CTest and expect PASS.

- [ ] **Step 6: Integrate startup loading and printing**

In `main()`, load `PdoBusConfig` from `options.config_path`, build `LoadedSlaveConfig`, and pass `loaded.view()` to `EthercatMaster::initialize()`. Print the mapping after registration has populated offsets and before IPC/realtime setup. Remove the old compile-time-config branch.

Move the existing one-line identity print out of `EthercatMaster::initialize()` so configuration output has one owner. Link yaml-cpp and `src/pdo_config.cpp` into `rocos_igh_core` in master mode.

- [ ] **Step 7: Compile both modes**

Run both standard configure/build commands. Expected: both complete successfully; no hardware process is started.

### Task 4: User-Facing Format Specification And Final Verification

**Files:**
- Modify: `docs/pdo-config.md`
- Modify: `README.md`
- Create: `config/pdo.yaml`

**Interfaces:**
- Consumes: exact YAML keys and CLI behavior implemented in Tasks 1-3.
- Produces: deployable example configuration and complete format reference.

- [ ] **Step 1: Write the format specification**

Document the complete example, field types/ranges, required fields, unknown-key rejection, `id == list index`, SII identity behavior, SM2/SM3 defaults, Rx/Tx direction meanings, entry-count ABI limits, and error behavior. Retain the existing interpolation-period SDO note as a separate section.

- [ ] **Step 2: Add the repository example and README command**

Create `config/pdo.yaml` with the currently compiled single-drive mapping. Update prerequisites to include yaml-cpp and show:

```bash
./build-master/rocos_igh_master --config config/pdo.yaml --master-id 0 --period-us 1000
```

- [ ] **Step 3: Run final executable validation**

Run:

```bash
cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF
cmake --build build
ctest --test-dir build --output-on-failure
cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON
cmake --build build-master
ctest --test-dir build-master --output-on-failure
```

Expected: all builds and tests pass. Do not start the master or perform SDO/device state changes as part of automated validation.

- [ ] **Step 4: Inspect diagnostics and diff**

Check compiler/editor diagnostics for all touched source files, then run `git diff --check` and review that every changed line traces to YAML loading, startup integration, tests, or documentation.