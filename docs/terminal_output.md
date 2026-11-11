# 启动终端输出规范

## 输出顺序与范围

启动诊断按照 `[Startup]` → `[Config]` → `[SHM]` → `[License]` → `[SHM] Process image` → `[PDO]` 输出。SDK 自身的状态、总线扫描和错误日志可能穿插其中。

- 配置加载并校验成功后，立即打印启动信息和配置摘要，早于 OS、实时环境和网卡初始化。配置失败仍输出 `Configuration error (<path>): <reason>` 并退出。
- License 在 `emSetLicenseKey` 返回后打印；PDO 在初始化共享内存映射时打印，早于发布 ready，包括目标状态为 INIT 的启动。
- 这些一次性诊断直接输出到标准输出，不受 `master.verbose` 过滤；`silent` 仍显示启动诊断。SDK 日志继续使用原有等级。需要完整留档时由部署环境捕获 stdout/stderr，不能只依赖 `master.log` 的 SDK 日志文件。
- 新增打印位于非实时初始化路径，不在 `EcMasterJobTask`、周期回调或定时线程中增加输出。

## 排版与颜色

- 使用稳定的 `[模块]` 前缀，便于搜索；配置行缩进两个空格，键名最小宽度 25，左对齐，以 ` : ` 分隔值。
- 启动及配置分节使用青色加粗；授权成功使用绿色加粗，未授权使用红色加粗。每段结束恢复终端颜色，不使用闪烁或背景色。
- 颜色由现有 termcolor 按终端能力处理；重定向时新增彩色段输出纯文本。授权状态同时包含 `AUTHORIZED` / `UNAUTHORIZED` 和中文说明，不依赖颜色传递结论。
- 分隔线使用 ASCII 字符。长路径和完整 PDO 名称不人为截断，允许终端自然换行；查看完整表格建议使用宽终端或 `less -S`。
- 时间、容量、偏移必须标注单位。索引、地址和类型码用十六进制；计数、位长、偏移用十进制。

## 启动信息与配置

`[Startup]` 包含项目完整版本（含 rc）、主站后端、SDK 编译版本、配置文件绝对路径。当前程序固定使用 `ecm (Acontis EC-Master; backend_type=1)`，并非通过配置选择 IgH。以后增加后端时应显示实际后端，不从文件名或网卡驱动名推断。

`[Config]` 列出加载后的全部 YAML 字段及默认值。键名保留 `master.*`，便于与 YAML 对照。枚举同时显示名称和内部数值，例如 `polling (1)`、`bus_shift (1)`；ENI 和日志前缀显示基于 YAML 所在目录解析后的路径。

特殊值的含义：

| 字段 | 展示规则 |
| --- | --- |
| `master.id` | 共享内存命名空间 ID，与网卡序号区分 |
| `master.instance` | 从 1 开始的网卡实例序号 |
| `master.duration` | 单位 ms；0 标注 `until stopped`，仍受授权、错误和外部停止约束 |
| `master.sp` / `master.mbxsrv` | 未显式启用时显示 `disabled`；启用时显示请求端口，不表示服务已启动成功 |
| `master.log` | 空值显示 `<empty> (SDK default prefix)` |
| `master.license` | 仅显示是否配置，隐藏完整密钥；授权结果在后续单独打印 |
| `master.maxbusslaves` | SDK 容量，不代表共享内存可容纳的从站数量 |
| `master.command_timeout_ms` | 单位 ms，并提示当前 SDO 只支持 PREOP |
| `output flashing` / `auxclk` | 明确标记内部默认值，不作为新增 YAML 配置项 |

`[SHM]` 显示 rocos-shm **布局 ABI 版本**（不是库的软件包版本）、头部和总线结构字节数、从站/PDO 容量，以及当前 ID 对应的总线、Input、Output、同步对象名称。随后取得 SDK 过程映像大小时，再打印两个方向实际需要的字节数。

示例（路径、版本和参数以实际构建及配置为准）：

```text
[Startup] ROCOS EtherCAT Master ==================================
  version                   : 3.1.0-rc.11
  backend                   : ecm (Acontis EC-Master; backend_type=1)
  SDK                       : <SDK version>
  config                    : /opt/rocos/ecm/config/config.yaml
[Config] Loaded settings (defaults included) --------------------
  master.id                 : 0 (IPC namespace)
  master.eni                : /opt/rocos/ecm/config/eni7.xml
  ...
[SHM] rocos-shm protocol ----------------------------------------
  ABI version               : 2
  header / bus size         : 80 / 220088 bytes
  capacity                  : 50 slaves; 25 Input / 25 Output variables per slave
  bus                       : /ecm_v2_0
  PDO Input / Output        : /pd_input_v2_0 / /pd_output_v2_0
  sync                      : /sync_v2_0_{0..9}
================================================================
```

## License

按当前项目约定，`emSetLicenseKey` 返回 `EC_E_NOERROR` 即作为启动授权成功，绿色打印：

```text
[License] AUTHORIZED - 已授权：可连续运行。
          emSetLicenseKey accepted the key (0x00000000).
```

拒绝密钥时红色打印错误文字和返回码，示例：

```text
[License] UNAUTHORIZED - 未授权：试用运行一小时后停止。
          emSetLicenseKey rejected the key: <SDK error text> (0x98110039)
```

“一小时”为试用运行提示，实际停止由 SDK 决定；本次打印改动不增加计时器，不修改 SDK 授权校验。已授权仍遵循配置的 `duration` 及正常停止条件，现有运行时 `EC_E_EVAL_EXPIRED` 清除授权标志并停止的处理保留。

## 按从站显示 PDO

使用 SDK 返回的拓扑顺序和实际站地址，不假设地址为 `1001 + 序号`。每个从站先打印名称、从 0 开始的 SHM 序号、站地址、SDK slave ID 和 ENI 身份，再分别打印 Input / Output。

```text
[PDO] Configured slaves=1; connected=1
      Input = slave -> master (TxPDO); Output = master -> slave (RxPDO)
      Offsets are absolute within each direction's process image.

[PDO] Slave[0] Drive | station=1001 (0x03e9), SDK slave ID=0
      ENI identity: vendor=0x00000001 product=0x00000002 revision=0x00000001 serial=0x00000000
  Input / TxPDO (1 variables) ------------------------------------------------
    #  PDO    Index  Sub  BitLength  Offset(bit)  Byte.Bit  Type    Name
    0  0x1a00 0x6041 0x00        16            0     0.0   0x0006  Drive.Statusword
  Output / RxPDO (0 variables) ------------------------------------------------
    (none)
[PDO] Mapping validated; end of PDO configuration.
```

| 列 | 含义 |
| --- | --- |
| `#` | 当前从站、当前方向内的变量序号，从 0 开始，与 SHM 变量索引一致 |
| `PDO` | SDK `wPdoIndex`，所属 PDO 的映射对象索引 |
| `Index` / `Sub` | SDK `wIndex` / `wSubIndex`，变量的对象字典索引/子索引 |
| `BitLength` | SDK `nBitSize`，单位 bit，保留原始位长 |
| `Offset(bit)` | SDK `nBitOffs`，在对应方向整个过程映像中的绝对位偏移，不是从站内相对偏移 |
| `Byte.Bit` | 位偏移除以 8 的商与余数，例如 `12.3` 表示第 12 字节的 bit 3；不是小数 |
| `Type` | SDK `wDataType`，EtherCAT 数据类型码 |
| `Name` | SDK 提供的完整变量名称，保留设备和 PDO 前缀；SHM 仍保存原有短名称 |

Input 是从站发给主站的 TxPDO；Output 是主站发给从站的 RxPDO。零变量方向显式显示 `(none)`。身份行标记为 `ENI identity`，避免把配置身份误当成实机读取结果。SDK 未提供的字段按其返回值显示，零值不补造对象索引。

打印发生在映射校验过程中：即使位段无法映射到当前只支持字节对齐的 SHM ABI，也先显示该条原始元数据，再打印 `[PDO] ERROR` 并保留原有失败行为。只有通过全部映射与布局校验才出现 `Mapping validated`；该行不代表从站已进入 OP 或硬件时序验证通过。

## 实现入口与验证

- `Main/Common/Linux/EcDemoMain.cpp`：`PrintStartupConfig()`，配置加载后的启动摘要。
- `Main/ECM/EcDemoApp.cpp`：License、过程映像大小；移除旧的全局顺序 PDO 打印调用。
- `Main/ECM/EcShmV2.cpp`：`initialize()` 复用现有 SDK 变量查询，按从站/方向打印，不增加逐变量查询。

修改后至少编译 `rocos_ecm`，配置及适配器离线验证限定运行 `config_test`、`shm_adapter_test`。终端检查应覆盖授权成功/失败、颜色重定向、零 PDO、完整长名称、非字节对齐条目的错误诊断。离线替代 SDK 验证不等同于真实硬件授权时长、OP 通信或实时性能验收。


## rocos-igh 适配

rocos-igh 使用相同的分节顺序、25 字符键宽、ASCII 分隔线、青色标题和绿色授权提示。`console_style.hpp` 根据 TTY、`TERM=dumb` 和 `NO_COLOR` 控制颜色，重定向输出为纯文本。

- `[Startup]` 显示 `VERSION` 完整版本、`igh (IgH EtherCAT Master; backend_type=2)`、构建时 ecrt 头文件的 API 版本和 YAML 绝对路径。
- `[Config]` 显示 IgH 支持的启动参数及默认值，使用 `master.*` 展示名。通用参数以 `master.id/cycle/cpuidx/eni` 展示，旧 YAML 的 `master.period_us` 对应 `master.cycle`，旧 `master.cpu` 对应 `master.cpuidx`。CLI 覆盖值附带原配置值。逐从站保留 DC 与启动 SDO 配置摘要。
- `[SHM]` 从 rocos-shm 常量和实际结构体取得布局 ABI、容量和当前主站的 IPC 名称。IgH 激活并取得 domain 大小后打印 Input/Output 字节数。
- `[License]` 显示 `AUTHORIZED` 和中文说明，并标明 IgH 固定 `is_authorized=true`、无需许可证密钥。
- `[PDO]` 按 SHM 从站序号输出 Input/TxPDO、Output/RxPDO。`position` 为 IgH 零基物理位置，`alias` 使用十六进制。身份来源标记为 `ENI identity` 或 `SII identity`。连接数来自 IgH 扫描快照，查询失败或扫描未结束时显示 `unknown`。
- PDO 表格复用已注册的映射索引、对象索引、子索引、位长、方向内绝对位偏移和 `Byte.Bit`，变量序号与各方向 SHM 索引一致。类型列显示 `n/a`，说明 IgH PDO 注册接口未提供 EtherCAT 数据类型。名称按加载后的完整字符串打印。
- YAML/ENI 在解析阶段拒绝非字节对齐条目时，错误信息先列出原始 PDO/对象索引、子索引、位长和名称，再输出 `[PDO] ERROR`，偏移标记为注册前不可用。注册成功的条目在过程映像布局校验前打印。`Mapping validated` 在 PDO 范围、共享元数据和 SHM 布局校验均通过后、发布 ready 前输出。

实现入口为 `src/startup_report.cpp`、`src/slave_config.cpp`、`src/ethercat_master.cpp` 和 `src/main.cpp`。新增输出均在非实时初始化路径。

离线预览可执行 `./build-master/startup_report_test --preview`，其中扫描结果和 PDO 偏移为测试样例。运行 `ctest --test-dir build --output-on-failure` 与 `ctest --test-dir build-master --output-on-failure` 验证两种构建模式。真实设备的 OP 通信与周期性能需要独立硬件验收。
