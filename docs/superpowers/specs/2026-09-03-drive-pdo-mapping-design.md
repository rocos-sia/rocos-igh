# Drive PDO Mapping Design

## Scope

Configure one EtherCAT drive at alias 0, ring position 0 from the object list in
`docs/pdo-config.md`. The configuration does not restrict the drive to a
compile-time vendor ID or product code. It reads both values from the scanned
SII information before creating the IgH slave configuration.

The interpolation-period object `0x60C2:02` is not written in this change. The
document does not specify its encoded value, and SDO configuration is separate
from PDO mapping.

## PDO Layout

SM2 maps master outputs through RxPDO `0x1600`, with the watchdog enabled:

1. `0x607A:00`, Target Position, 32 bits
2. `0x60FF:00`, Target Velocity, 32 bits
3. `0x6071:00`, Target Torque, 16 bits
4. `0x6040:00`, Control Word, 16 bits
5. `0x6060:00`, Modes of Operation, 8 bits

SM3 maps master inputs through TxPDO `0x1A00`:

1. `0x6041:00`, Status Word, 16 bits
2. `0x6064:00`, Position Actual Value, 32 bits
3. `0x606C:00`, Velocity Actual Value, 32 bits
4. `0x6077:00`, Torque Actual Value, 16 bits
5. `0x20A0:00`, Auxiliary Position Actual Value, 32 bits
6. `0x2205:02`, Analog Input, 16 bits

## Identity Resolution

`vendor_id == 0` and `product_code == 0` together mean that initialization must
read the identity for the configured ring position with
`ecrt_master_get_slave()`. The discovered non-zero pair is written back to the
mutable `SlaveSpec` before calling `ecrt_master_slave_config()`. A half-wildcard
pair remains invalid.

Before activation, the process prints the resolved alias, position, vendor ID,
and product code once on the non-real-time startup path. No logging or SII access
is added to the cyclic path.

## Validation

The master-mode unit test verifies the exact default PDO order, direction,
sub-index, and bit width, as well as the all-zero identity placeholder. Existing
configuration and metadata tests continue to cover explicit non-zero identities.
Actual SII discovery and PDO application require the opt-in hardware environment.