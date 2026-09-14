# DC Initial Application Time Design

## Problem

When Distributed Clocks are enabled, `EthercatMaster::initialize()` activates
the IgH master before the cyclic task supplies application time. The caller then
creates IPC objects and configures the realtime environment before entering the
cycle. During this gap, the IgH master state machine can reach DC system-time
initialization with `dc_ref_time == 0`, skip writing the slaves' 64-bit System
Time Offset registers, and request OP.

On the connected seven Elmo drives this left register `0x0920` at zero while
SYNC0 start times were calculated from the much larger `CLOCK_MONOTONIC` value.
SYNC0 was therefore scheduled far in the future and every drive rejected OP
with AL status `0x002D` (`No Sync Error`). The transient domain working-counter
values from `1/7` through `6/7` reflected IgH's sequential slave configuration,
not permanently missing PDO configurations.

## Selected Approach

Seed IgH application time at the master activation boundary when DC is enabled:

1. Read `CLOCK_MONOTONIC` immediately before `ecrt_master_activate()`.
2. Call `ecrt_master_application_time()` with that timestamp so the asynchronous
   IgH state machine sees a nonzero application/reference time as soon as the
   master becomes active.
3. Activate the master.
4. Read `CLOCK_MONOTONIC` again and refresh application time immediately after
   activation, before domain pointer lookup or any caller-side IPC/realtime
   setup.
5. Keep the existing per-cycle application-time, reference-clock, and
   slave-clock synchronization calls unchanged.

The pre-activation call is intentionally an initialization seed for the local
IgH 1.6.12 implementation: `ecrt_master_application_time()` only stores
`app_time` and initializes `dc_ref_time`, without issuing a datagram. Periodic
phase timing remains driven by the established realtime cycle.

## Code Structure

Add a small activation helper in `ethercat_master.cpp`. It accepts injected
clock, application-time, and activation operations so its ordering and failure
behavior can be tested without requesting EtherCAT hardware. Production uses
`clock_gettime`, `ecrt_master_application_time`, and `ecrt_master_activate`.
The helper remains private to `EthercatMaster`, exposed to tests only through
the existing `EthercatMasterTestPeer` friendship.

`EthercatMaster::initialize()` invokes the helper after DC configuration and
reference-clock selection. With DC disabled, the helper calls only activation,
preserving current behavior.

## Error Handling

- Failure to read the initial monotonic timestamp aborts initialization with a
  message identifying the pre-activation DC time seed.
- Failure of either initial `ecrt_master_application_time()` call aborts
  initialization and records the application-time DC error stage.
- Activation failure retains the existing `failed to activate EtherCAT master`
  diagnostic.
- Every failure follows the existing `reset()` path so master ownership and
  borrowed pointers are released consistently.

No blocking calls, allocation, or new logging are added to the 1 ms cyclic
path.

## Tests

Hardware-free unit coverage will verify:

- DC disabled performs activation without reading or applying application time.
- DC enabled orders operations as `clock -> application time -> activate ->
  clock -> application time` and forwards both timestamps exactly.
- Clock, application-time, and activation failures stop at the correct step and
  produce the expected diagnostic/error stage.

Build verification will run both documented configurations. The existing
unrelated `pdo_config_test` baseline failure (checked-in example entry-count
expectation) will be reported separately and will not be treated as introduced
by this change.

With explicit user authorization, hardware verification will gracefully stop
the currently running master, start the rebuilt binary with
`config/talon_pdo.yaml --dc on`, and verify:

- all seven slaves reach OP;
- both domains reach working counter `7/7`;
- the slaves receive a nonzero 64-bit System Time Offset;
- no new AL status `0x002D` messages appear after the new process starts.

If hardware verification fails, the process will be stopped and the captured
kernel/master diagnostics will be reported without writing SDOs, forcing AL
states, or changing the Elmo configuration.

## Non-Goals

- Changing the Elmo DC parameters in `talon_pdo.yaml`.
- Patching the IgH kernel module.
- Fixing the pre-existing `pdo_config_test` example mismatch.
- Changing EoE configuration or mailbox behavior.
