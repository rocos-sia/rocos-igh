# Shared-memory Viewer Typed Values Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a per-PDO data-type selector that decodes live little-endian bytes as the selected compatible integer or float type.

**Architecture:** Put deterministic byte decoding and compatible-type lookup in a small ES module shared by the browser and Node tests. Keep selection state in the page script, keyed by slave, direction, object identity, and offset, then use delegated change handling so SSE redraws retain the chosen type.

**Tech Stack:** HTML/CSS, browser ES modules, JavaScript `DataView`, Node.js built-in test runner, CMake.

## Global Constraints

- Do not change the server JSON API or the shared-memory ABI.
- Only expose `UINT8`/`INT8` for one byte, `UINT16`/`INT16` for two bytes, and `UINT32`/`INT32`/`FLOAT` for four bytes.
- Decode multi-byte values as little-endian.
- Keep selections only for the current page lifetime.
- Preserve the hexadecimal byte display before the decoded value.

---

### Task 1: Typed PDO value rendering

**Files:**
- Create: `tools/shm_web/value_decoder.mjs`
- Create: `tools/shm_web/value_decoder.test.mjs`
- Modify: `tools/shm_web/index.html`
- Modify: `tools/shm_web/CMakeLists.txt`

**Interfaces:**
- Produces: `typesForSize(size)` returning compatible type names.
- Produces: `decodeValue(hexBytes, type)` returning a JavaScript number.
- Produces: `formatTypedBytes(hexBytes, type)` returning `"AA BB = value"`.

- [ ] **Step 1: Write failing decoder tests**

Cover compatible options for 1/2/4/unsupported byte sizes; little-endian unsigned and signed values; and IEEE-754 single-precision decoding with Node's `node:test` and `node:assert/strict`.

- [ ] **Step 2: Run the decoder test and verify RED**

Run: `node --test tools/shm_web/value_decoder.test.mjs`

Expected: FAIL because `value_decoder.mjs` does not exist.

- [ ] **Step 3: Implement the decoder module**

Parse the compact hexadecimal string into a `Uint8Array`, wrap it in a `DataView`, and dispatch to `getUint8`, `getInt8`, `getUint16`, `getInt16`, `getUint32`, `getInt32`, or `getFloat32`. Pass `true` for every multi-byte read.

- [ ] **Step 4: Run the decoder test and verify GREEN**

Run: `node --test tools/shm_web/value_decoder.test.mjs`

Expected: all decoder tests PASS.

- [ ] **Step 5: Add the selector to the live table**

Import the decoder module from `index.html`, add the `数据类型` header after `实时值`, render only options returned by `typesForSize`, and default to the matching unsigned type. Store selections in a `Map` keyed by slave ID, direction, index, sub-index, and offset. Add one delegated `change` listener on `#slaves` that records the selection and updates the row value immediately.

- [ ] **Step 6: Copy the browser module during build**

Extend the existing CMake post-build command to copy `value_decoder.mjs` beside `index.html` and `shm_web_server`.

- [ ] **Step 7: Validate tests and build**

Run: `node --test tools/shm_web/value_decoder.test.mjs`

Run: `cmake -S tools/shm_web -B tools/shm_web/build && cmake --build tools/shm_web/build`

Expected: tests pass and the viewer builds with both frontend assets copied beside the executable.

- [ ] **Step 8: Validate in demo mode**

Start `tools/shm_web/build/shm_web_server --demo --port 8484`, open `http://127.0.0.1:8484`, and confirm each row exposes only width-compatible types, signed/float selections change the decoded value, and the selection remains selected after at least one SSE refresh.