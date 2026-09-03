<!--
 Copyright (c) 2026 'Yang Luo, luoyang@sia.cn'
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# rocos-igh

Minimal C++ EtherCAT master shell built on IgH EtherCAT Master, with shared-memory IPC for client processes.

## Build and test (hardware-free)

Use this mode when IgH development files are not installed. It builds the IPC-focused library and tests.

```bash
cmake -S . -B build -DROCOS_IGH_BUILD_MASTER=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

## Build and test (master enabled)

Use this mode when `ecrt.h` and `libethercat` are available on the machine.

```bash
cmake -S . -B build-master -DROCOS_IGH_BUILD_MASTER=ON
cmake --build build-master
ctest --test-dir build-master --output-on-failure
```

## Runtime entrypoint

The executable is `rocos_igh_master` and supports:

- `--master-id <non-negative integer>`
- `--period-us <integer >= 1000>`
- `--help`

The checked-in configuration maps one drive at alias 0, ring position 0. It uses
RxPDO `0x1600` on SM2 and TxPDO `0x1A00` on SM3 with the entries documented in
[PDO configuration](docs/pdo-config.md).

The drive identity is not fixed at compile time. Startup reads the vendor ID and
product code from the scanned SII data, prints both values, and then passes the
resolved identity to IgH for strict matching. Startup fails before creating IPC
or applying realtime privileges when the slave at position 0 is unavailable.

## Additional docs

- [Architecture](docs/architecture.md)
- [API guide](docs/api-guide.md)
- [EtherCAT terminal commands](docs/ethercat-terminal-commands.md)

