# Shared Memory Web Workbench Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restyle the shared-memory observer as a compact gray engineering workbench with blue controls while preserving all live-data behavior.

**Architecture:** Keep the dependency-free single-page architecture and existing JavaScript rendering flow. Restructure only the static HTML shell and CSS in `tools/shm_web/index.html`; retain every JavaScript-owned DOM ID and API endpoint so the C++ server and value decoder remain unchanged.

**Tech Stack:** HTML5, CSS, browser-native JavaScript modules, Server-Sent Events, Node.js test runner, CMake/C++17 server.

## Global Constraints

- Preserve the existing single-page, dependency-free implementation.
- Preserve all current DOM IDs, SSE and polling behavior, snapshot rendering, status handling, and typed PDO decoding.
- Use graphite gray as the dominant surface color and electric blue for interactive controls and active states.
- Keep state semantics green for healthy/live, amber for warning/waiting, and red for offline/error.
- Use compact corners of 6px or less; do not add decorative gradients or oversized typography.
- Do not add configuration controls, write access, charts, dependencies, or backend API changes.
- Do not commit changes unless the user explicitly requests a commit.

---

### Task 1: Lock The Frontend Contract

**Files:**
- Create: `tools/shm_web/index.contract.test.mjs`
- Test: `tools/shm_web/index.contract.test.mjs`

**Interfaces:**
- Consumes: `tools/shm_web/index.html` as UTF-8 text.
- Produces: a Node.js contract test that verifies required live-data IDs and responsive/table styling markers remain present.

- [ ] **Step 1: Add a failing workbench contract test**

Create a Node test using `node:test`, `node:assert/strict`, and `readFileSync`. Assert that `index.html` contains the existing IDs `dot`, `statustext`, `state`, `reqstate`, `auth`, `slavenum`, `dt`, `cur`, `min`, `max`, `avg`, `ts`, `mode`, `reset`, and `slaves`. Also assert the intended workbench hooks `workbench-bar`, `metrics-strip`, and `table-scroll`, plus a mobile media query and visible `:focus-visible` styling.

- [ ] **Step 2: Run the test and verify the new workbench assertions fail**

Run: `node --test tools/shm_web/index.contract.test.mjs`

Expected: FAIL because `workbench-bar`, `metrics-strip`, and `table-scroll` are not yet present in the current page.

- [ ] **Step 3: Keep the test focused on observable page contracts**

Do not assert exact color hex values or entire markup blocks. The test should catch accidental removal of runtime-owned elements and required responsive hooks without making future visual tuning brittle.

---

### Task 2: Build And Verify The Engineering Workbench

**Files:**
- Modify: `tools/shm_web/index.html`
- Test: `tools/shm_web/index.contract.test.mjs`
- Test: `tools/shm_web/value_decoder.test.mjs`

**Interfaces:**
- Consumes: existing IDs and the `handle(snapshot)` rendering flow in `index.html`.
- Produces: `workbench-bar`, `metrics-strip`, and per-slave `table-scroll` containers while preserving all existing data selectors and event handlers.

- [ ] **Step 1: Replace the visual tokens and base typography**

Define neutral graphite workspace and panel variables, a bright blue interactive variable, semantic state colors, a local `IBM Plex Sans`-first UI stack, and the existing technical monospace stack. Add a subtle technical grid texture using CSS backgrounds while keeping the page predominantly gray.

- [ ] **Step 2: Restructure the static shell**

Replace the separate title/subtitle/status composition with a compact `.workbench-bar`. Convert the three `.card` groups into a single `.metrics-strip` containing three semantic metric groups. Keep every existing JavaScript-owned ID exactly once.

- [ ] **Step 3: Restyle dynamic slave panels**

Update the template emitted by `renderSlaves()` so each PDO table is enclosed in `<div class="table-scroll">`. Style slave headers, tables, tags, byte values, and selectors as dense engineering controls with blue hover/focus treatment and stable column widths.

- [ ] **Step 4: Add responsive behavior**

At desktop widths, use a compact three-column metrics strip and full-width slave panels. Below tablet width, wrap the workbench bar, reduce page padding, collapse metrics to one column, and allow `.table-scroll` to scroll horizontally with a fixed table minimum width.

- [ ] **Step 5: Run the focused contract test**

Run: `node --test tools/shm_web/index.contract.test.mjs`

Expected: PASS with all required IDs and workbench/responsive hooks present.

- [ ] **Step 6: Run the existing decoder regression test**

Run: `node --test tools/shm_web/value_decoder.test.mjs`

Expected: PASS with no typed-value decoding regressions.

- [ ] **Step 7: Build the standalone server and copied assets**

Run: `cmake -S tools/shm_web -B tools/shm_web/build && cmake --build tools/shm_web/build`

Expected: exit code 0; `shm_web_server`, `index.html`, and `value_decoder.mjs` are available in `tools/shm_web/build`.

- [ ] **Step 8: Validate the live page in demo mode**

Run `./tools/shm_web/build/shm_web_server --demo --port 8484`, then inspect `http://127.0.0.1:8484` at approximately 1440x900 and 390x844. Confirm live SSE values change, selectors update values, tables scroll without overlap, status states remain readable, and no browser console errors appear.