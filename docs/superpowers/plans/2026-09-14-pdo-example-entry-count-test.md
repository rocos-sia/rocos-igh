# PDO Example Entry Count Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Correct the stale checked-in PDO example assertions so the complete test suite reflects the intended seven-entry RxPDO and five-entry TxPDO layout.

**Architecture:** Change only the exact-count assertions in the existing parser integration test. Keep `config/pdo.yaml`, parser behavior, EtherCAT runtime code, and the validated DC activation fix unchanged; then verify both build modes before and after the local merge.

**Tech Stack:** C++17, CMake, CTest, Git.

---

### Task 1: Correct the checked-in example expectations

**Files:**
- Modify: `tests/pdo_config_test.cpp:300-314`

- [ ] **Step 1: Re-run the existing regression test in its failing state**

Run:

```bash
ctest --test-dir build -R '^pdo_config$' --output-on-failure
```

Expected: FAIL at `tests/pdo_config_test.cpp:312` because the parsed RxPDO has seven entries while the assertion expects three.

- [ ] **Step 2: Update only the two stale exact-count assertions**

Replace:

```cpp
    CHECK(config.slaves[0].rx_pdos[0].entries.size() == 3U);
    CHECK(config.slaves[0].tx_pdos[0].entries.size() == 3U);
```

with:

```cpp
    CHECK(config.slaves[0].rx_pdos[0].entries.size() == 7U);
    CHECK(config.slaves[0].tx_pdos[0].entries.size() == 5U);
```

- [ ] **Step 3: Rebuild and verify the corrected regression test**

Run:

```bash
cmake --build build --target pdo_config_test -j2
ctest --test-dir build -R '^pdo_config$' --output-on-failure
```

Expected: `pdo_config` passes with zero failures.

- [ ] **Step 4: Commit the test correction**

```bash
git add tests/pdo_config_test.cpp
git commit -m "test: update PDO example entry counts"
```

### Task 2: Verify the complete feature branch

**Files:**
- No source-file changes.

- [ ] **Step 1: Build and test hardware-free mode**

Run:

```bash
cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Expected: both `pdo_config` and `shared_memory_config` pass.

- [ ] **Step 2: Build and test master mode after the first suite has exited**

Run sequentially, not in parallel with Step 1, because both test binaries use global POSIX IPC names:

```bash
cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON
cmake --build build-master -j2
ctest --test-dir build-master --output-on-failure
```

Expected: both `pdo_config` and `shared_memory_config` pass.

- [ ] **Step 3: Verify branch cleanliness and scope**

Run:

```bash
git diff --check
git status --short
git log --oneline --decorate -8
```

Expected: no uncommitted files; the new test commit changes only `tests/pdo_config_test.cpp`.

### Task 3: Merge the verified branch into `main`

**Files:**
- Merge commits from: `fix/dc-initial-application-time`
- Preserve user-owned modification: `config/talon_pdo.yaml`

- [ ] **Step 1: Stop the validation process before removing its worktree**

The process was started in persistent execution session `90707`. Send `Ctrl+C` to that session and verify PID `1830536` exits:

```text
write_stdin(session_id=90707, chars="\u0003")
```

Then run:

```bash
ps -p 1830536 -o pid=,stat=,comm=,args=
ethercat master
```

Expected: PID `1830536` is absent and Master0 reports `Active: no`.

- [ ] **Step 2: Confirm main-workspace state and fast-forward merge**

From `/home/sun/Documents/GitHub/rocos-igh`, run:

```bash
git status --short
git branch --show-current
git merge --ff-only fix/dc-initial-application-time
```

Expected: the current branch is `main`; the user-owned `config/talon_pdo.yaml` modification remains present and does not conflict; `main` advances to the feature branch tip.

- [ ] **Step 3: Reconfigure, rebuild, and test the merged result sequentially**

From `/home/sun/Documents/GitHub/rocos-igh`, run:

```bash
cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF
cmake --build build -j2
ctest --test-dir build --output-on-failure
cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON
cmake --build build-master -j2
ctest --test-dir build-master --output-on-failure
```

Expected: both tests pass in both build modes.

- [ ] **Step 4: Remove the owned worktree and delete the merged branch**

From `/home/sun/Documents/GitHub/rocos-igh`, run:

```bash
git worktree remove /home/sun/Documents/GitHub/rocos-igh/.worktrees/dc-initial-time
git worktree prune
git branch -d fix/dc-initial-application-time
```

Expected: the isolated worktree and merged feature branch are removed; `main` retains all commits.

### Task 4: Restore and revalidate the merged master process

**Files:**
- No source-file changes.

- [ ] **Step 1: Capture the new log boundary and start from the main workspace**

From `/home/sun/Documents/GitHub/rocos-igh`, capture:

```bash
date --iso-8601=seconds
```

Then start the merged executable in a persistent terminal:

```bash
./build-master/rocos_igh_master \
  --config /home/sun/Documents/GitHub/rocos-igh/config/talon_pdo.yaml \
  --dc on
```

- [ ] **Step 2: Verify the restored bus state**

Run:

```bash
ethercat slaves
ethercat domains
ethercat reg_read -p 0 0x0920 8
```

Expected: all seven slaves are `OP+`, both domains report `WorkingCounter 7/7`, and register `0x0920` is nonzero.

- [ ] **Step 3: Verify final repository state**

Run:

```bash
git status --short
git log --oneline --decorate -8
```

Expected: `main` points at the merged feature tip; only the pre-existing user modification to `config/talon_pdo.yaml` remains uncommitted.

