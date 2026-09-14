# DC Initial Application Time Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Seed and immediately refresh IgH application time around master activation so DC-enabled Elmo drives receive a valid 64-bit clock offset and can reach OP.

**Architecture:** Keep all existing PDO/DC configuration and realtime-cycle calls intact. Add one private, dependency-injected activation helper to `EthercatMaster`; production adapters read `CLOCK_MONOTONIC` and call IgH, while hardware-free master-mode tests inject deterministic operations and verify exact ordering and failures.

**Tech Stack:** C++17, IgH EtherCAT Master 1.6 API, POSIX `clock_gettime`, CMake/CTest.

---

### Task 1: Specify activation-boundary behavior with failing tests

**Files:**
- Modify: `tests/shared_memory_config_test.cpp:26-67`
- Modify: `tests/shared_memory_config_test.cpp:820-930`
- Modify: `tests/shared_memory_config_test.cpp:940-1010`

- [ ] **Step 1: Expose the private helper through the existing test peer**

Add this method to `rocos::EthercatMasterTestPeer`:

```cpp
    static bool activateWithInitialApplicationTime(
        EthercatMaster &master,
        bool dc_enabled,
        std::string &error,
        const std::function<int(std::uint64_t &)> &read_time,
        const std::function<int(std::uint64_t)> &apply_time,
        const std::function<int()> &activate) {
        return master.activateWithInitialApplicationTime(
            dc_enabled, error, read_time, apply_time, activate);
    }
```

- [ ] **Step 2: Add an exact-order test for enabled and disabled DC**

Add the following test before `testMasterCyclicCallsBeforeInitialization()`:

```cpp
bool testActivationSeedsDcApplicationTime() {
    rocos::EthercatMaster master;
    std::vector<std::string> calls;
    const std::uint64_t times[] = {1000000001ULL, 1000000999ULL};
    std::size_t next_time = 0;
    std::vector<std::uint64_t> applied_times;
    std::string error;

    const auto read_time = [&](std::uint64_t &value) {
        calls.emplace_back("clock");
        value = times[next_time++];
        return 0;
    };
    const auto apply_time = [&](std::uint64_t value) {
        calls.emplace_back("application_time");
        applied_times.push_back(value);
        return 0;
    };
    const auto activate = [&] {
        calls.emplace_back("activate");
        return 0;
    };

    CHECK(rocos::EthercatMasterTestPeer::activateWithInitialApplicationTime(
        master, true, error, read_time, apply_time, activate));
    CHECK(error.empty());
    CHECK(calls == std::vector<std::string>({
        "clock", "application_time", "activate", "clock", "application_time"
    }));
    CHECK(applied_times == std::vector<std::uint64_t>({times[0], times[1]}));

    calls.clear();
    next_time = 0;
    applied_times.clear();
    CHECK(rocos::EthercatMasterTestPeer::activateWithInitialApplicationTime(
        master, false, error, read_time, apply_time, activate));
    CHECK(error.empty());
    CHECK(calls == std::vector<std::string>({"activate"}));
    CHECK(next_time == 0U);
    CHECK(applied_times.empty());
    return true;
}
```

- [ ] **Step 3: Add failure-path tests**

Add this test beside the exact-order test:

```cpp
bool testActivationDcFailuresStopAtFailingOperation() {
    rocos::EthercatMaster master;
    std::string error;
    std::size_t clock_calls = 0;
    std::size_t application_calls = 0;
    std::size_t activation_calls = 0;

    const auto good_clock = [&](std::uint64_t &value) {
        ++clock_calls;
        value = 42U + clock_calls;
        return 0;
    };
    const auto good_application = [&](std::uint64_t) {
        ++application_calls;
        return 0;
    };
    const auto good_activation = [&] {
        ++activation_calls;
        return 0;
    };

    CHECK(!rocos::EthercatMasterTestPeer::activateWithInitialApplicationTime(
        master, true, error,
        [](std::uint64_t &) { return EIO; }, good_application, good_activation));
    CHECK(error.find("CLOCK_MONOTONIC before master activation") != std::string::npos);
    CHECK(application_calls == 0U);
    CHECK(activation_calls == 0U);

    clock_calls = application_calls = activation_calls = 0;
    CHECK(!rocos::EthercatMasterTestPeer::activateWithInitialApplicationTime(
        master, true, error, good_clock,
        [&](std::uint64_t) { ++application_calls; return -EIO; },
        good_activation));
    CHECK(error.find("DC application time before master activation") != std::string::npos);
    CHECK(master.lastDcError().stage == rocos::DcErrorStage::ApplicationTime);
    CHECK(master.lastDcError().error_code == -EIO);
    CHECK(clock_calls == 1U);
    CHECK(application_calls == 1U);
    CHECK(activation_calls == 0U);

    clock_calls = application_calls = activation_calls = 0;
    CHECK(!rocos::EthercatMasterTestPeer::activateWithInitialApplicationTime(
        master, true, error, good_clock, good_application,
        [&] { ++activation_calls; return -EIO; }));
    CHECK(error == "failed to activate EtherCAT master");
    CHECK(clock_calls == 1U);
    CHECK(application_calls == 1U);
    CHECK(activation_calls == 1U);

    clock_calls = application_calls = activation_calls = 0;
    CHECK(!rocos::EthercatMasterTestPeer::activateWithInitialApplicationTime(
        master, true, error,
        [&](std::uint64_t &value) {
            ++clock_calls;
            value = 100U + clock_calls;
            return clock_calls == 2U ? EIO : 0;
        }, good_application, good_activation));
    CHECK(error.find("CLOCK_MONOTONIC after master activation") != std::string::npos);
    CHECK(clock_calls == 2U);
    CHECK(application_calls == 1U);
    CHECK(activation_calls == 1U);

    clock_calls = application_calls = activation_calls = 0;
    CHECK(!rocos::EthercatMasterTestPeer::activateWithInitialApplicationTime(
        master, true, error, good_clock,
        [&](std::uint64_t) {
            ++application_calls;
            return application_calls == 2U ? -EIO : 0;
        }, good_activation));
    CHECK(error.find("DC application time after master activation") != std::string::npos);
    CHECK(master.lastDcError().stage == rocos::DcErrorStage::ApplicationTime);
    CHECK(master.lastDcError().error_code == -EIO);
    CHECK(clock_calls == 2U);
    CHECK(application_calls == 2U);
    CHECK(activation_calls == 1U);
    return true;
}
```

Register both functions in the master-only block of `main()` immediately before `testMasterCyclicCallsBeforeInitialization()`:

```cpp
    if (!testActivationSeedsDcApplicationTime()) {
        return EXIT_FAILURE;
    }
    if (!testActivationDcFailuresStopAtFailingOperation()) {
        return EXIT_FAILURE;
    }
```

- [ ] **Step 4: Run the master-mode test build and confirm the new API is absent**

Run:

```bash
cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON
cmake --build build-master --target shared_memory_config_test -j2
```

Expected: compilation fails because `EthercatMaster` has no member named `activateWithInitialApplicationTime`.

### Task 2: Seed application time around master activation

**Files:**
- Modify: `src/ethercat_master.hpp:102-116`
- Modify: `src/ethercat_master.cpp:5-12`
- Modify: `src/ethercat_master.cpp:194-214`
- Modify: `src/ethercat_master.cpp:220-260`

- [ ] **Step 1: Declare the private dependency-injected helper**

Add this declaration before `recordDcError()` in `EthercatMaster`:

```cpp
    bool activateWithInitialApplicationTime(
        bool dc_enabled,
        std::string &error,
        const std::function<int(std::uint64_t &)> &read_time,
        const std::function<int(std::uint64_t)> &apply_time,
        const std::function<int()> &activate);
```

- [ ] **Step 2: Implement ordered seeding and diagnostics**

Add this definition after the injected `waitForSlavesInPreop()` overload:

```cpp
bool EthercatMaster::activateWithInitialApplicationTime(
    bool dc_enabled,
    std::string &error,
    const std::function<int(std::uint64_t &)> &read_time,
        const std::function<int(std::uint64_t)> &apply_time,
        const std::function<int()> &activate) {
    error.clear();
    last_dc_error_ = DcError{};
    const auto seed_application_time = [&](const char *position) {
        std::uint64_t application_time = 0;
        const int clock_result = read_time(application_time);
        if (clock_result != 0) {
            error = std::string("failed to read CLOCK_MONOTONIC ") + position +
                    " master activation (error " +
                    std::to_string(clock_result) + ")";
            return false;
        }
        const int application_result = apply_time(application_time);
        if (application_result != 0) {
            recordDcError(DcErrorStage::ApplicationTime, application_result);
            error = std::string("failed to set DC application time ") + position +
                    " master activation (error " +
                    std::to_string(application_result) + ")";
            return false;
        }
        return true;
    };

    if (dc_enabled && !seed_application_time("before")) {
        return false;
    }
    if (activate() != 0) {
        error = "failed to activate EtherCAT master";
        return false;
    }
    if (dc_enabled && !seed_application_time("after")) {
        return false;
    }
    return true;
}
```

- [ ] **Step 3: Replace direct activation with POSIX/IgH production adapters**

Add `<cerrno>` and `<ctime>` to `ethercat_master.cpp`, then replace the direct `ecrt_master_activate()` block with:

```cpp
    const auto read_application_time = [](std::uint64_t &application_time) {
        timespec now{};
        if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return errno;
        }
        application_time = static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
                           static_cast<std::uint64_t>(now.tv_nsec);
        return 0;
    };
    const auto apply_application_time = [this](std::uint64_t application_time) {
        return ecrt_master_application_time(master_, application_time);
    };
    const auto activate_master = [this] {
        return ecrt_master_activate(master_);
    };
    if (!activateWithInitialApplicationTime(
            dc_runtime.enabled, error, read_application_time,
            apply_application_time, activate_master)) {
        const DcError dc_error = last_dc_error_;
        reset();
        last_dc_error_ = dc_error;
        return false;
    }
```

This keeps the existing activation error text and preserves an initialization-time application error across `reset()` while still releasing the requested master and all borrowed pointers.

- [ ] **Step 4: Build and run the focused master-mode test**

Run:

```bash
cmake --build build-master --target shared_memory_config_test -j2
ctest --test-dir build-master -R '^shared_memory_config$' --output-on-failure
```

Expected: build succeeds and `shared_memory_config` passes.

- [ ] **Step 5: Commit the tested implementation**

```bash
git add src/ethercat_master.hpp src/ethercat_master.cpp tests/shared_memory_config_test.cpp
git commit -m "fix: seed DC time during master activation"
```

### Task 3: Document the DC startup exception and ordering

**Files:**
- Modify: `docs/api-guide.md:32-61`
- Modify: `docs/api-guide.md:493-514`

- [ ] **Step 1: Update the activation overview**

Add the DC-only seed steps around activation in the overview:

```text
│  5c. DC 启用时: ecrt_master_application_time() 播种初始应用时间             │
│  6. ecrt_master_activate()   结束配置、进入运行阶段                         │
│  6a. DC 启用时: 立即刷新 ecrt_master_application_time()                    │
```

- [ ] **Step 2: Explain why both initial calls are required**

Replace the DC example and notes with:

```cpp
// 激活前:先配置各从站 DC、选择参考时钟并播种应用时间
ecrt_slave_config_dc(sc, assign, sync0_cycle, sync0_shift,
                     sync1_cycle, sync1_shift);
ecrt_master_select_reference_clock(dc_ref_sc);
ecrt_master_application_time(master, monotonic_time_ns);

ecrt_master_activate(master);
ecrt_master_application_time(master, refreshed_monotonic_time_ns);

// 运行期,每个周期顺序调用:
ecrt_master_application_time(master, app_time);
ecrt_master_sync_reference_clock(master);
ecrt_master_sync_slave_clocks(master);
```

Add these exact notes immediately below the example:

```markdown
- 本机 IgH 1.6.12 实现会在首次收到非零应用时间时初始化内部
  `dc_ref_time`。ROCOS 因此在激活前播种一次，并在激活返回后、获取域缓冲区和
  创建 IPC 之前立即刷新一次，避免异步状态机在 `dc_ref_time == 0` 时跳过从站
  64 位 System Time Offset 初始化。
- 激活前调用是针对该 IgH 实现的启动期例外；激活后的周期调用仍是正常的
  `master_op, rt_safe` 用法，并保持在实时循环的固定位置。
- 当前周期顺序为 application time → receive/process → PDO 复制/状态 →
  domain queue → reference/slave clock sync → send。
- 配置期 DC 调用失败会中止启动；运行期 DC 调用失败会停止周期任务并返回
  `EIO`。
```

- [ ] **Step 3: Review and commit the documentation**

Run:

```bash
git diff --check
```

Expected: no output and exit status 0.

Then commit:

```bash
git add docs/api-guide.md
git commit -m "docs: describe DC activation time seeding"
```

### Task 4: Verify software behavior in both build modes

**Files:**
- No source-file changes.

- [ ] **Step 1: Run the hardware-free build mode**

Run:

```bash
cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Expected: `shared_memory_config` passes. Record the already-observed `pdo_config` example entry-count failure separately if it remains; this task does not alter that fixture or parser.

- [ ] **Step 2: Run the master build mode**

Run:

```bash
cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON
cmake --build build-master -j2
ctest --test-dir build-master --output-on-failure
```

Expected: compilation succeeds and `shared_memory_config` passes. If `pdo_config` still fails at the checked-in example entry-count assertion, compare it with the recorded baseline and report it as pre-existing.

- [ ] **Step 3: Inspect the final diff and commit graph**

Run:

```bash
git diff --check
git status --short
git log --oneline --decorate -5
```

Expected: no uncommitted implementation or documentation changes; commits contain only the approved DC fix, tests, and documentation.

### Task 5: Validate the rebuilt master on the authorized seven-drive bus

**Files:**
- No source-file changes.

- [ ] **Step 1: Resolve and gracefully stop only the current ROCOS master**

Run read-only process inspection first:

```bash
pgrep -a -f 'rocos_igh_master.*talon_pdo.yaml'
```

Confirm the matching command is the currently running master for this bus. Send `SIGINT` to that exact PID and poll until it exits. Do not use a broad process pattern for the signal and do not force-kill it.

- [ ] **Step 2: Capture the kernel-log boundary and start the rebuilt binary**

Capture an exact wall-clock boundary, inspect the latest kernel entry, then start:

```bash
validation_since="$(date --iso-8601=seconds)"
journalctl -k -n 1 --output=short-monotonic --no-pager
```

Start the rebuilt master:

```bash
sudo ./build-master/rocos_igh_master \
  --config /home/sun/Documents/GitHub/rocos-igh/config/talon_pdo.yaml \
  --dc on
```

Keep the process attached to a persistent terminal session so startup failures and the exact process lifetime remain observable.

- [ ] **Step 3: Verify slave and domain state without writing device state**

After configuration settles, run:

```bash
ethercat slaves
ethercat domains -v
```

Expected: all seven slaves report `OP`; both input and output domains report working counter `7/7` and complete WC state.

- [ ] **Step 4: Verify DC offset and absence of new synchronization errors**

Read the reference drive's System Time Offset register and inspect kernel messages after the captured log boundary:

```bash
ethercat reg_read -p 0 0x0920 8
journalctl -k --since "$validation_since" --no-pager
```

Expected: register `0x0920` is nonzero and the new startup interval contains no AL status `0x002D` / `No Sync Error` messages. The register command is read-only; do not issue SDO downloads, register writes, or forced state transitions.

- [ ] **Step 5: Handle a failed hardware check safely**

If any expected condition is not met, send `SIGINT` to the exact rebuilt master PID, wait for it to exit, and report the observed slave/domain/register/kernel state. Leave the Elmo configuration unchanged.
