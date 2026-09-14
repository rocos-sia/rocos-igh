# PDO Example Entry Count Test Design

## Problem

Commit `92adaeb` intentionally expanded `config/pdo.yaml` from three RxPDO and
three TxPDO entries to seven RxPDO and five TxPDO entries. The
`testLoadsCheckedInExample()` assertions were not updated, so both documented
build modes fail at the stale RxPDO count even though the parser loads the file
correctly.

## Selected Approach

Keep the checked-in YAML and parser unchanged. Update the example test's exact
entry-count expectations from `3/3` to `7/5`. Exact counts retain stronger
regression coverage than merely checking that the lists are nonempty and match
the current example's intended PDO layout.

## Testing and Scope

The existing failing `pdo_config` CTest is the regression test: it must change
from failure at `tests/pdo_config_test.cpp:312` to passing. Afterward, run the
complete CTest suite in both `ROCOS_IGH_BUILD_MASTER=OFF` and `ON` modes.

This change does not modify PDO parsing, runtime configuration, the Elmo
`talon_pdo.yaml`, EtherCAT device state, or the DC activation fix.
