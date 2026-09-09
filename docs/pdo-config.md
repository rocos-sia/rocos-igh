# PDO YAML 配置规范

主站通过启动参数 `--config <path>` 加载 PDO 配置。配置文件使用 YAML，当前
格式版本为 `1`。

```bash
./build-master/rocos_igh_master \
  --config config/pdo.yaml \
  --master-id 0 \
  --period-us 1000
```

文件不可读取、YAML 语法错误、缺少字段或字段值非法时，主站会在请求
EtherCAT 主站和创建共享内存之前退出。错误信息包含文件路径和尽可能精确的
字段路径。

## 完整示例

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

仓库中的 [示例配置](../config/pdo.yaml) 与上述内容一致。

## 字段说明

| 路径 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `version` | 无符号整数 | 必须为 `1` | 配置格式版本 |
| `slaves` | 序列 | 1 到 50 项 | 按 EtherCAT 总线物理顺序排列的从站 |
| `slaves[].id` | 无符号 16 位整数 | 从 `0` 连续递增，必须等于序列下标 | 从站编号及物理位置 |
| `slaves[].name` | 字符串 | 非空，最多 79 字节 | 终端和共享内存中显示的从站名称 |
| `slaves[].rx_pdos` | 序列 | 非空 | 从站接收、主站输出的 PDO 列表 |
| `slaves[].tx_pdos` | 序列 | 非空 | 从站发送、主站输入的 PDO 列表 |
| `rx_pdos[].index` / `tx_pdos[].index` | 无符号 16 位整数 | `0` 到 `0xFFFF` | PDO 映射对象索引 |
| `entries` | 序列 | 非空 | 当前 PDO 的对象条目，顺序即映射顺序 |
| `entries[].name` | 字符串 | 非空，最多 71 字节 | 终端和共享内存中显示的变量名称 |
| `entries[].index` | 无符号 16 位整数 | `0` 到 `0xFFFF` | 对象字典索引 |
| `entries[].sub_index` | 无符号 8 位整数 | `0` 到 `255` | 对象字典子索引 |
| `entries[].bit_length` | 无符号 8 位整数 | `8` 到 `248`，且为 8 的倍数 | PDO 条目位宽 |

整数可以写成十进制或带 `0x` 前缀的十六进制，不得添加引号、正负号或空白。
所有字段都是必填字段，重复字段和未在上表列出的字段都会被拒绝，以避免
歧义或拼写错误被静默忽略。

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

