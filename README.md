<!--
 Copyright (c) 2026 'Yang Luo, luoyang@sia.cn'
 
 This software is released under the MIT License.
 https://opensource.org/licenses/MIT
-->

# rocos-igh

Minimal C++ EtherCAT master shell built on IgH EtherCAT Master, with shared-memory IPC for client processes.

The build requires yaml-cpp (`libyaml-cpp-dev` on Debian/Ubuntu). Master-enabled
builds additionally require the IgH `ecrt.h` header and `libethercat` library.

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

- `--config <PDO YAML path>` (required)
- `--master-id <non-negative integer>`
- `--period-us <integer >= 1000>`
- `--help`

```bash
./build-master/rocos_igh_master --config config/pdo.yaml --master-id 0 --period-us 1000
```

The YAML file lists slaves in physical bus order and configures each slave's
RxPDO and TxPDO mappings. Alias is fixed to zero; slave IDs start at zero and
also define ring positions. See the full [PDO configuration](docs/pdo-config.md)
format specification and the checked-in [example](config/pdo.yaml).

Slave identities are not configured in YAML. Startup reads each vendor ID and
product code from SII, prints the resolved mapping, and passes the actual
identity to IgH for strict matching.

## Additional docs

- [Architecture](docs/architecture.md)
- [API guide](docs/api-guide.md)
- [EtherCAT terminal commands](docs/ethercat-terminal-commands.md)

