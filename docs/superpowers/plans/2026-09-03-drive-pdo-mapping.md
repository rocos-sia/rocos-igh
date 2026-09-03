# Drive PDO Mapping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Configure one CiA 402-style drive with the PDO objects documented in `docs/pdo-config.md`, resolving its identity from SII at startup.

**Architecture:** `slave_config.cpp` owns the compile-time SM/PDO tables and returns one mutable default `SlaveSpec`. `EthercatMaster::initialize()` resolves an all-zero identity from the scanned slave and reports it before passing exact values to IgH and activating the master.

**Tech Stack:** C++17, IgH EtherCAT userspace API, CMake, CTest

## Global Constraints

- Keep the shared `EcatBus`, `Slave`, and `PdVar` ABI unchanged.
- Use RxPDO `0x1600` on SM2 and TxPDO `0x1A00` on SM3.
- Perform SII discovery and all output before master activation and the cyclic loop.
- Do not configure `0x60C2:02` without a specified encoded interpolation-period value.
- Preserve support for explicit non-zero vendor ID and product code pairs.

---

### Task 1: Default PDO Table

**Files:**
- Modify: `tests/shared_memory_config_test.cpp`
- Modify: `src/slave_config.cpp`
- Modify: `src/slave_config.hpp`

**Interfaces:**
- Consumes: `StaticSlaveConfig defaultSlaveConfig() noexcept`
- Produces: one default `SlaveSpec` with 11 ordered `PdoEntrySpec` records and complete IgH sync tables

- [ ] **Step 1: Add a failing master-mode test**

Add `testDefaultSlavePdoMapping()` and assert one slave, identity `0/0`, five
outputs, six inputs, and literal object/sub-index/bit-width values in document
order. The production change that makes this test pass is replacing the empty
default configuration with the requested static table.

- [ ] **Step 2: Verify the test fails**

Run the existing master test target. Expected: failure at
`config.slave_count == 1` because the current default is empty.

- [ ] **Step 3: Add the minimal static mapping**

Define `ec_pdo_entry_info_t` arrays for the five RxPDO and six TxPDO entries,
two `ec_pdo_info_t` records at `0x1600` and `0x1A00`, and an `ec_sync_info_t`
table for SM2/SM3 plus the `0xff` terminator. Define matching mutable
`PdoEntrySpec` records and return one position-zero slave from
`defaultSlaveConfig()`.

- [ ] **Step 4: Re-run the focused test**

Run the master test target. Expected: the new mapping test passes; identity
validation may still fail until Task 2 permits the all-zero sentinel.

---

### Task 2: Runtime SII Identity

**Files:**
- Modify: `tests/shared_memory_config_test.cpp`
- Modify: `src/slave_config.cpp`
- Modify: `src/ethercat_master.cpp`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `ecrt_master_get_slave(ec_master_t *, uint16_t, ec_slave_info_t *)`
- Produces: all-zero identity sentinel resolution before `ecrt_master_slave_config()`

- [ ] **Step 1: Add failing identity validation cases**

Assert that `0/0` is structurally valid and that `0/non-zero` and
`non-zero/0` are rejected. The production change that makes this pass is an
all-or-none identity rule.

- [ ] **Step 2: Verify the tests fail**

Run the master test target. Expected: the all-zero default is rejected by the
existing non-zero validation.

- [ ] **Step 3: Implement identity resolution**

After requesting the master and before creating domains, call
`ecrt_master_get_slave()` for each `0/0` slave, reject unavailable or zero SII
identities, and write the discovered values back to `SlaveSpec`. Explicit
non-zero identity pairs bypass discovery.

- [ ] **Step 4: Display the resolved identity**

Before activation, print each configured slave's alias, position, vendor ID, and
product code in hexadecimal. Restore decimal formatting afterward.

- [ ] **Step 5: Run complete verification**

Configure, build, and run CTest in no-hardware mode. When IgH development files
are available, also build and run the master-mode unit test. Hardware validation
must confirm that startup prints the SII identity and the slave reaches process
data exchange without a PDO configuration error.