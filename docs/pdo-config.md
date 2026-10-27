# PDO YAML 配置规范

主站通过启动参数 `--config <path>` 加载 PDO 配置。配置文件使用 YAML，当前
格式版本为 `1`。同一文件的可选 `master` 块可配置主站启动参数；
命令行显式参数覆盖 YAML，未配置字段使用程序默认值。`--config` 始终由命令行指定。

```bash
./build-master/rocos_igh_master \
  --config config/pdo.yaml \
  --master-id 0 \
  --period-us 1000 \
  --dc off
```

文件不可读取、YAML 语法错误、缺少字段或字段值非法时，主站会在请求
EtherCAT 主站和创建共享内存之前退出。错误信息包含文件路径和尽可能精确的
字段路径。

## 完整示例

```yaml
version: 1

master:                       # 可选；此处列出程序默认值
  master_id: 0
  period_us: 1000
  cpu: null                   # null 沿用原有亲和性；整数指定逻辑 CPU
  dc: "off"
  preop_timeout_ms: 10000
  op_timeout_ms: 60000
  pdo_config_attempts: 5

slaves:
  - id: 0
    name: EtherCAT Drive
    dc_ref: true      # DC 参考从站；其他从站 false 或省略
    init_commands: []  # 可选 PREOP SDO 初始化列表
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

仓库中的 [示例配置](../config/pdo.yaml) 使用相同 PDO 映射，并通过
`dc_ref: true` 指定参考时钟。

## 字段说明

| 路径 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `version` | 无符号整数 | 必须为 `1` | 配置格式版本 |
| `master` | 映射 | 可选 | 主站启动参数，所有子字段均可省略 |
| `master.master_id` | 无符号整数 | 默认 `0` | 对应 `--master-id` |
| `master.period_us` | 无符号 32 位整数 | `>= 1000`，默认 `1000` | 对应 `--period-us`，单位 μs |
| `master.cpu` | 无符号整数或 null | 默认 `null` | 对应 `--cpu`；`null` 沿用亲和性，CLI `--cpu auto` 可取消 YAML 绑定 |
| `master.dc` | 字符串 | `"on"` / `"off"`，默认 `"off"` | 对应 `--dc`，控制是否为全部从站启用 DC |
| `master.preop_timeout_ms` | 无符号 32 位整数 | `>= 1`，默认 `10000` | 对应 `--preop-timeout-ms`，包含全部从站 PREOP 配置及读回 |
| `master.op_timeout_ms` | 无符号 32 位整数 | `>= 1`，默认 `60000` | 对应 `--op-timeout-ms` |
| `master.pdo_config_attempts` | 无符号 32 位整数 | `>= 1`，默认 `5` | 对应 `--pdo-config-attempts`，每从站总尝试次数，包含首次 |
| `slaves` | 序列 | 1 到 50 项 | 按 EtherCAT 总线物理顺序排列的从站 |
| `slaves[].id` | 无符号 16 位整数 | 从 `0` 连续递增，必须等于序列下标 | 从站编号及物理位置 |
| `slaves[].name` | 字符串 | 非空，最多 79 字节 | 终端和共享内存中显示的从站名称 |
| `slaves[].init_commands` | 序列 | 可选，默认 `[]` | PREOP 下按顺序写入的 SDO 命令，不嵌套 `entries` |
| `init_commands[].name` | 字符串 | 可选；非空，最多 71 字节 | 配置中的说明标签，不加入 PDO/共享内存变量 |
| `init_commands[].index` | 无符号 16 位整数 | 必填，`0..0xFFFF` | 对象字典索引 |
| `init_commands[].sub_index` | 无符号 8 位整数 | 必填，`0..255` | 对象字典子索引 |
| `init_commands[].Type` | 字符串 | 必填，支持下表的 8 种整数类型 | 自动决定写入长度；也可写作 `type` |
| `init_commands[].Value` | 整数 | 必填，必须落在 Type 的范围内 | 也可写作 `value`；不接受加引号的数字 |
| `slaves[].dc_ref` | 布尔值 | 可选，默认 `false` | `true` 指定 DC 参考时钟；启用 DC 时恰好一个为 true |
| `slaves[].rx_pdos` | 序列 | 非空 | 从站接收、主站输出的 PDO 列表 |
| `slaves[].tx_pdos` | 序列 | 非空 | 从站发送、主站输入的 PDO 列表 |
| `rx_pdos[].index` / `tx_pdos[].index` | 无符号 16 位整数 | `0` 到 `0xFFFF` | PDO 映射对象索引 |
| `entries` | 序列或空值 | 必填，可为空 | 非空时为显式映射；空时启动通过 SDO 读取该 PDO 的当前映射并写回 YAML。推荐 `[]`，也接受 `{}`、`null` 或留空的 `entries:` |
| `entries[].name` | 字符串 | 非空，最多 71 字节 | 终端和共享内存中显示的变量名称 |
| `entries[].index` | 无符号 16 位整数 | `0` 到 `0xFFFF` | 对象字典索引 |
| `entries[].sub_index` | 无符号 8 位整数 | `0` 到 `255` | 对象字典子索引 |
| `entries[].bit_length` | 无符号 8 位整数 | `8` 到 `248`，且为 8 的倍数 | EtherCAT 线上实际位宽；ABI v2 仅接受字节对齐条目 |

无符号整数可以写成十进制或带 `0x` 前缀的十六进制，不得添加引号、正负号或空白。
有符号初始化值允许十进制负数。
除明确标为可选的块和字段外，所有字段都是必填字段。重复字段和未在上表列出
的字段都会被拒绝，以避免歧义或拼写错误被静默忽略。

同一个方向可以混合空 PDO 和非空 PDO；`rx_pdos` / `tx_pdos` 本身仍须为非空序列。
非空 `entries` 必须使用条目序列，不能使用映射或标量。

## 空 entries 自动补全

例如 `rx_pdos: [{index: 0x1600, entries: []}]` 表示从物理位置 `id` 的从站读取
`0x1600:00`（8 位条目数），再逐项读取 `0x1600:01..N`（32 位映射描述）。
每个描述按高 16 位对象索引、中间 8 位子索引、低 8 位位宽解码。
这里的“默认配置”指启动时从站当前保存的映射，不执行恢复出厂设置；不会自动替换
YAML 中填写的 PDO 索引或扫描其他 PDO。已有非空 `entries` 保持不变。

名称采用自动递增形式：`RxPDO_1600_01`、`RxPDO_1600_02`、`TxPDO_1A00_01` 等，
序号从 1 开始，不依赖 SII 是否提供名称。比如读到 `0x70000110` 后写回：

```yaml
entries: [{name: RxPDO_1600_01, index: 0x7000, sub_index: 1, bit_length: 16}]
```

所有空 PDO 均读取成功并通过位宽、每从站每方向条目数上限校验后，才统一写回
`--config` 指定的文件，并在本次启动立即使用补全结果构建 IgH 配置及共享内存变量。
位宽必须非零；支持 1 位 GPIO 以及跨字节的映射。YAML 和 IgH 配置始终使用实际
位宽，例如 `0x70000101` 表示 `0x7000:01` 的 1 位映射，16 位映射应为 `0x70000110`。
从站实际条目数为 0 时保存为 `[]`，不生成变量，下次启动仍会尝试读取。

写回仅替换对应的空 `entries` 值，保留周围注释、其他配置及文件权限；使用同目录
临时文件校验并原子替换。读取失败、响应长度错误、超时、从站离开健康 PREOP、
映射超出 ABI 限制、文件被同时修改或无法写回时，中止启动，不保存部分发现结果。
特殊标签或锚点定义形式请先改为普通 `[]`、`{}`、`null` 或 `entries:`。
CLI 覆盖的启动参数不写回 YAML。写回发生在后续初始化命令和主站激活之前；
后续启动失败不会撤销已保存的发现结果。

PREOP 配置先读回现有映射；已经与 YAML 一致的映射跳过清空和重写，以支持只读
映射及补全后的再次启动。不一致的非空映射沿用原有重配置流程；PDO 分配对象仍
按原流程写入和校验。真正为空的映射不写入映射对象，但仍要求读回条目数为 0。
所有 PDO 仍接受 PREOP 最终读回和激活后启动阶段的异步验证。

## ABI v2 的字节 PDO 限制

按[新共享内存规范](shared-memory-v2.md)，非整字节位宽或 IgH 注册后起始位不为 0
的 PDO 明确报错，不再拆位/合位或分配额外字节槽。按字节对齐的变量保持 domain
字节偏移；`sizeof(T)` 必须与 `PdVar.size` 相等。字节偏移可能不满足 C++ 类型对齐，
此时使用基于 `memcpy` 的 value API，不能使用 typed-pointer API。

`config/tc7_pdo.yaml` 第 8 个从站的 1-bit GPIO 是实际设备映射；此文件保留供核对，
但不能由当前 ABI v2 主站启动。不得将 `bit_length: 1` 改为 8 来绕过限制。
自动发现非字节映射也会失败，不把不兼容发现结果写回 YAML。

周期超时跳过已错过周期并累计计数。启动验证期间掉线或 WKC 异常仍中止启动；
运行期通过 `link_up`/`wc_state` 与有效位报告异常，继续周期通信，不自动重映射或
清零输出。IgH 的 `is_authorized` 表示许可结果，始终为 true，不作为总线健康判据。

## PREOP SDO 初始化命令

每个从站可添加 `init_commands` 列表，条目写法参考 PDO `entries`，但长度由
`Type` 确定，不填写 `bit_length`。例如（仅演示格式，需替换为目标设备支持的对象）：

```yaml
slaves:
  - id: 0
    name: Drive
    init_commands:
      - {name: Parameter A, index: 0x2000, sub_index: 0, Type: uint32, Value: 1000}
      - {name: Parameter B, index: 0x2001, sub_index: 1, Type: int16, Value: -2}
    # rx_pdos / tx_pdos 仍需按实际设备填写
```

| Type | 长度 | Value 范围 |
| --- | --- | --- |
| `int8` / `uint8` | 1 字节 | -128..127 / 0..255 |
| `int16` / `uint16` | 2 字节 | -32768..32767 / 0..65535 |
| `int32` / `uint32` | 4 字节 | -2147483648..2147483647 / 0..4294967295 |
| `int64` / `uint64` | 8 字节 | -9223372036854775808..9223372036854775807 / 0..18446744073709551615 |

类型名称使用小写。`Type`/`Value` 与 `type`/`value` 等价，但同一条目不能同时
给出两种拼写。Value 的非负值可用十进制或 `0x`/`0X` 十六进制；负值使用十进制。
有符号类型的十六进制数也必须在正数范围内，例如 int8 的 -1 应写 `-1`，不能写
`0xFF`。写入采用小端字节序，负数按指定位宽的补码编码。
当前不支持浮点、布尔、字符串、数组或 Complete Access。

启动执行顺序：等待 PREOP 稳定 → 读取空 `entries` 的当前映射、校验并写回 YAML →
按从站顺序执行全部 `init_commands`
（各列表内部保持顺序）→ 逐站完整配置并读回 RxPDO/TxPDO 分配和映射 →
全部 PDO 验证通过后激活 → 原有 OP 阶段的异步 PDO 再验证。
初始化命令使用 SDO 下载成功应答确认完成，不强制读回初始化对象，兼容只写对象。
同一对象可在列表中出现多次，按顺序执行，不合并、不去重。
PDO 最终内容由 `rx_pdos`/`tx_pdos` 决定；初始化命令写入的 PDO 分配/映射对象
会被随后的 PDO 配置覆盖。

每次初始化 SDO 前后检查目标从站仍处于无错误 PREOP；写入失败、状态异常或
超时立即终止启动，不激活、不继续执行后续命令。错误包含从站位置、对象/子索引、
长度以及可用的返回码和 SDO abort code。已成功写入的命令不会自动回滚。
初始化命令每次启动执行一次，不受 `pdo_config_attempts` 控制，也不会因 PDO 重试
而重复执行；需要重新启动才能重新执行整个初始化列表。

等待、默认映射读取及写回、初始化命令、PDO 配置和读回重试共同使用
`preop_timeout_ms` 总预算。
阻塞式 IgH SDO 调用仅在激活前执行，超时在调用返回后判定；不会将这些写入加入
1 ms 周期路径，也不在运行期掉线或重配置时自动重放命令。共享内存 ABI 不变。
未配置或配置为空列表时，保持原有启动行为。

## Distributed Clocks

主站级开关保持不变：`master.dc: on` 或命令行 `--dc on` 启用 DC，默认关闭；
命令行 `--dc off` 可临时覆盖 YAML 中的开启设置。
从站只配置 `dc_ref`，不再使用嵌套的 `dc` 块：

```yaml
slaves:
  - id: 0
    name: Reference Drive
    dc_ref: true
    # rx_pdos / tx_pdos 按实际设备填写
  - id: 1
    name: Other Drive
    dc_ref: false  # 也可省略
    # rx_pdos / tx_pdos 按实际设备填写
```

`dc_ref` 只决定参考时钟，不是各从站的同步开关。启用 DC 后，所有配置从站均使用
统一的 SYNC0 模式；`false` 或省略的从站仍参与同步。参考从站可以位于任意位置，
不要求是第一个。从站标记必须是未加引号的 `true` / `false`。
多个 `dc_ref: true` 会在 YAML 加载时拒绝；没有参考从站时允许关闭 DC 启动，
但启用 DC 会在请求主站前报错。

以下参数移为内部固定值，沿用仓库现有设备配置，无需在 YAML 填写：

- `assign_activate = 0x0300`，使用 SYNC0 模式。
- `sync0_shift_ns = 0`。
- `sync1_cycle_ns = 0`、`sync1_shift_ns = 0`，不使用 SYNC1。
- SYNC0 周期取合并后的 `period_us * 1000` 纳秒（命令行优先于 `master.period_us`）。

旧的 `slaves[].dc` 块及上述单独字段会作为未知字段拒绝，请删除并改为 `dc_ref`。
`config/pdo.yaml`、`config/talon_pdo.yaml`、`config/ZeroErr6.yaml` 已同步迁移。
这些固定设置对应本项目当前的 SYNC0 使用方式，其他模式需在代码中适配。

启用 DC 时若周期换算超过 IgH `uint32_t` 纳秒范围，启动参数校验失败。
配置从站或选择参考时钟失败会中止启动；运行期间 application time、参考时钟或
从站时钟同步调用失败会停止周期循环并返回 `EIO`。
CPU 可用性也在 YAML/命令行合并后校验；YAML 本身的非法值、重复或未知字段始终报错。

每个从站的所有 `rx_pdos` 条目总数不能超过 `MAX_PDOUTPUT_NUM`（当前为
25），所有 `tx_pdos` 条目总数不能超过 `MAX_PDINPUT_NUM`（当前为 25）。

## 固定和自动发现的参数

以下参数无需也不能在 YAML 中配置：

- `alias` 固定为 `0`。
- `position` 由 `id` 得出。
- `vendor_id` 和 `product_code` 在启动时从对应位置从站的 SII 读取。
- RxPDO 固定使用 SM2、`EC_DIR_OUTPUT`，并启用 watchdog。
- TxPDO 固定使用 SM3、`EC_DIR_INPUT`，并禁用 watchdog。

SII 返回的 vendor ID 或 product code 为零时，启动失败。身份读取完成后，
主站仍使用实际身份调用 IgH，从而保持严格匹配。

## 方向定义

- **RxPDO**：从站接收的数据，方向为 `master -> slave`，在共享内存中属于
  output 变量。
- **TxPDO**：从站发送的数据，方向为 `slave -> master`，在共享内存中属于
  input 变量。

成功注册 PDO 后，主站在终端按从站打印实际身份、PDO 索引、对象地址、位宽
和 domain 字节偏移。该输出发生在实时周期启动之前。

## 多从站配置

在 `slaves` 末尾继续添加从站即可。以下片段表示物理位置 1 的第二个从站：

```yaml
  - id: 1
    name: Second EtherCAT Drive
    rx_pdos:
      - index: 0x1600
        entries:
          - {name: Control Word, index: 0x6040, sub_index: 0, bit_length: 16}
    tx_pdos:
      - index: 0x1A00
        entries:
          - {name: Status Word, index: 0x6041, sub_index: 0, bit_length: 16}
```

`id` 不允许跳号或与列表顺序不一致。

## 插值周期配置

对象 `0x60C2:02` 用于配置插值周期，需要通过 SDO 写入。SDO 配置不属于本
PDO YAML 格式，主站当前不会根据此文件写入该对象。

