# Shared Memory Web Workbench Design

## Goal

Refresh `tools/shm_web/index.html` as a compact engineering workbench inspired by the visual language of robotsfan viewer. The interface should feel technical and precise, use gray as its dominant surface color, and reserve blue for interactive controls and active states.

## Scope

- Preserve the existing single-page, dependency-free HTML/CSS/JavaScript implementation.
- Preserve all current DOM IDs, SSE and polling behavior, snapshot rendering, status handling, and typed PDO decoding.
- Change only the page structure and presentation needed for the visual refresh.
- Do not add configuration controls, write access, charts, dependencies, or backend API changes.

## Visual Direction

- Use a graphite-gray workspace, medium-gray panels, subtle borders, and restrained shadows.
- Use electric blue for selectors, focus rings, active connection details, and other interactive emphasis.
- Keep state semantics distinct: green for healthy/live, amber for warning/waiting, and red for offline/error.
- Use a locally available industrial sans-serif font stack headed by `IBM Plex Sans`, with a technical monospace stack for measurements and addresses. No remote font dependency is required.
- Keep corners compact at 6px or less and avoid decorative gradients, oversized typography, and card-heavy marketing composition.

## Layout

1. A narrow top workbench bar contains the product identity, live connection indicator, and refresh/source context.
2. Bus state, cycle timing, and runtime metadata become a compact responsive metrics strip rather than three floating cards.
3. Each slave remains an independent full-width data panel with a dense header and scrollable PDO table.
4. Direction tags and state badges retain semantic colors while fitting the gray-blue system.
5. Data-type selectors receive a clearly blue interactive treatment, including keyboard focus states.

## Responsive Behavior

- The top bar and metrics strip wrap cleanly on narrow screens.
- Metrics use stable grid tracks on desktop and collapse to one or two columns on smaller viewports.
- PDO tables preserve column widths and scroll horizontally inside their panel rather than compressing or overlapping text.
- Controls remain comfortably targetable without increasing the density of the desktop layout.

## Data And Error Behavior

- Existing render functions and data flow remain unchanged unless a small markup adjustment is required by the layout.
- Available, waiting, reconnecting, demo, zero-slave, and unavailable states remain visible.
- Dynamic row updates must not rebuild tables when the slave schema is unchanged.
- Variable type selections remain stable during live updates.

## Verification

- Run the existing `value_decoder.mjs` Node.js tests.
- Build the standalone `tools/shm_web` CMake target to confirm assets are copied and the server still compiles.
- Launch demo mode and inspect desktop and mobile browser screenshots.
- Confirm SSE updates render, type selectors work, tables do not overlap, and the page remains readable in waiting/error states.