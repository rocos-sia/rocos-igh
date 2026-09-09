# YAML PDO Configuration Design

## Scope

Replace the compile-time PDO table with a required YAML configuration loaded
before requesting the EtherCAT master. Each configured slave has its own RxPDO
and TxPDO mappings. The existing shared-memory ABI and cyclic data path remain
unchanged.

The executable accepts a required `--config <path>` option. A missing option,
an unreadable file, invalid YAML, or an invalid PDO definition causes startup to
exit before EtherCAT activation and shared-memory creation.

## Configuration Format

```yaml
version: 1

slaves:
  - id: 0
    name: EtherCAT Drive
    rx_pdos:
      - index: 0x1600
        entries:
          - name: Target Position
            index: 0x607A
            sub_index: 0
            bit_length: 32
          - name: Digital Outputs
            index: 0x60FE
            sub_index: 0
            bit_length: 32
          - name: Control Word
            index: 0x6040
            sub_index: 0
            bit_length: 16

    tx_pdos:
      - index: 0x1A00
        entries:
          - name: Position Actual Value
            index: 0x6064
            sub_index: 0
            bit_length: 32
          - name: Digital Inputs
            index: 0x60FD
            sub_index: 0
            bit_length: 32
          - name: Status Word
            index: 0x6041
            sub_index: 0
            bit_length: 16
```

`version` must be `1`. `slaves` is a non-empty sequence ordered by physical bus
position. Each slave `id` must equal its zero-based sequence index, so `id` is
also the IgH slave position. Alias is always zero.

`name` is a required non-empty string used in terminal output and shared-memory
metadata. `rx_pdos` describes data received by the slave (master outputs), while
`tx_pdos` describes data transmitted by the slave (master inputs). Both are
non-empty sequences, and each slave may define multiple PDOs in either
direction.

PDO and entry `index` values accept decimal or YAML hexadecimal integer syntax.
`sub_index` is an unsigned 8-bit integer. `bit_length` is an unsigned 8-bit,
non-zero multiple of eight. Entry names must be non-empty and fit the existing
shared-memory ABI limits.

## Default EtherCAT Details

Fields that are discoverable or invariant are intentionally absent from YAML:

- `alias` is fixed to zero.
- `position` is derived from `id`.
- `vendor_id` and `product_code` are read from each slave's SII during startup.
- RxPDOs use SM2, `EC_DIR_OUTPUT`, and `EC_WD_ENABLE`.
- TxPDOs use SM3, `EC_DIR_INPUT`, and `EC_WD_DISABLE`.
- The Sync Manager table is terminated with the IgH `0xff` sentinel.

SII identity discovery must produce a non-zero vendor ID and product code before
calling `ecrt_master_slave_config()`.

## Components And Ownership

A YAML loader uses yaml-cpp to parse and validate the file into an owning
configuration object. This object owns all strings and vectors for the lifetime
of the master. After loading, it builds the IgH `ec_pdo_entry_info_t`,
`ec_pdo_info_t`, and `ec_sync_info_t` arrays plus the existing `PdoEntrySpec` and
`SlaveSpec` views.

The owning object must remain alive through master initialization and metadata
publication because the IgH and project structures contain pointers into its
storage. Pointer-bearing views are built only after vector sizes are final, so
later reallocation cannot invalidate them.

`EthercatMaster` continues to consume `StaticSlaveConfig`. This keeps YAML and
dynamic allocation out of the activation and cyclic interfaces and avoids any
change to `EcatBus`, `Slave`, `PdVar`, or their legacy aliases.

## Startup Flow

1. Parse `--master-id`, `--period-us`, and required `--config`.
2. Load and validate the YAML file without EtherCAT side effects.
3. Request the selected IgH master.
4. Read each slave identity from SII at position `id`.
5. Configure SM2/SM3 and all PDO mappings, then register entries into the input
   and output domains.
6. Print the resolved configuration once on the non-real-time startup path.
7. Activate the master, create IPC, publish metadata, and enter the cyclic loop.

No file access, YAML work, allocation, or configuration logging occurs after
the cyclic loop starts.

## Formatted Startup Output

After successful PDO registration, startup prints one structured section per
slave. It includes slave ID and name, discovered vendor/product IDs, and each
RxPDO/TxPDO entry with object address, bit width, and registered domain byte
offset. For example:

```text
Slave 0: EtherCAT Drive
  Identity: vendor=0x000000ab product=0x00001234
  RxPDO 0x1600 (SM2, master -> slave)
    0x607a:00  32 bit  offset=0  Target Position
  TxPDO 0x1a00 (SM3, slave -> master)
    0x6064:00  32 bit  offset=0  Position Actual Value
```

Errors include the configuration path and a field location such as
`slaves[1].tx_pdos[0].entries[2].bit_length` wherever practical.

## Validation

Tests run without EtherCAT hardware and cover:

- Parsing a valid multi-slave, multi-PDO configuration.
- Decimal and hexadecimal integer forms.
- Missing required fields and unsupported `version`.
- Non-contiguous or out-of-order slave IDs.
- Empty names, PDO lists, and entry lists.
- Numeric overflow and non-byte-aligned entry widths.
- Existing per-slave shared-memory input/output entry limits.
- Required `--config` parsing and usage text.

The normal no-hardware CMake build and CTest suite must pass. Master-mode
compilation verifies yaml-cpp and IgH type integration; applying mappings to
real devices remains an opt-in hardware integration test.

## Documentation And Dependency

`docs/pdo-config.md` becomes the user-facing format specification, including
the complete schema, field table, constraints, direction terminology, startup
command, and examples for one and multiple slaves.

CMake locates yaml-cpp as a required dependency for the master configuration
loader and links it through `rocos_igh_core`. The README documents the package
prerequisite and the required `--config` invocation.