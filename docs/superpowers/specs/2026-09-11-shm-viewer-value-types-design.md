# Shared-memory viewer typed values

## Scope

Add a data-type selector after the live-value column for every PDO variable in
the shared-memory web viewer. Keep the server API and shared-memory ABI
unchanged; conversion happens entirely in the browser from the existing raw
little-endian bytes.

## Behavior

- One-byte variables offer `UINT8` and `INT8`.
- Two-byte variables offer `UINT16` and `INT16`.
- Four-byte variables offer `UINT32`, `INT32`, and `FLOAT`.
- Other widths show `N/A` and keep their hexadecimal bytes without a decoded
  value.
- The default is the unsigned integer matching the variable width.
- The live-value cell keeps the hexadecimal byte display and shows the decoded
  value after `=`.
- `FLOAT` uses IEEE-754 single-precision little-endian decoding.
- A variable's selection survives SSE table redraws for the current page
  lifetime. It is not persisted across page reloads.

Selections are keyed by slave ID, direction, object index/sub-index, and byte
offset so input and output variables cannot overwrite one another.

## Implementation

Use browser `DataView` methods to decode a byte array parsed from the snapshot's
hexadecimal string. Render the selector with only width-compatible options and
handle its `change` event through delegation on the slave table container. A
change updates the in-memory selection map and re-renders the selected row's
live value immediately; subsequent snapshots use the saved selection.

## Validation

Automated checks cover little-endian unsigned, signed, and floating-point
decoding, compatible menu options, and stable variable keys. Browser validation
uses the server's demo mode to confirm that selectors render, changing a type
updates the value, and the selection remains after SSE refreshes.