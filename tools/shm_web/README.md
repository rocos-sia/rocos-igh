# shm_web —— rocos_igh 共享内存实时观察器（Web）

一个零依赖的小工具：**C++17 后端**（读懂 POSIX 共享内存）+ **纯 HTML/JS 前端**（浏览器实时展示）。
复用 [src/shared_memory_config.hpp](../../src/shared_memory_config.hpp) 的客户端路径连接到主站进程创建的共享内存，
把总线快照与 PDO 变量值以 JSON 暴露，并用 Server-Sent Events 周期推送，实现"动态显示"。

## 目录

- `shm_web_server.cpp` —— HTTP 后端：静态文件 + `/api/snapshot`（JSON 快照）+ `/api/events`（SSE 推送）。
- `index.html` —— 前端单页：总线状态卡片、周期统计、每个从站的输入/输出 PDO 变量实时值。
- `value_decoder.mjs` —— 小端 PDO 数据类型解析与写入编码，供浏览器页面与 Node.js 测试复用。
- `CMakeLists.txt` —— 构建脚本（独立于主工程，可单独编译）。

## 构建

```bash
git submodule update --init --recursive
cmake -S tools/shm_web -B tools/shm_web/build
cmake --build tools/shm_web/build
```

生成 `tools/shm_web/build/shm_web_server`，并把 `index.html` 复制到其旁。

## 用法

**方式一：观察真实主站**（需要先运行 `rocos_igh_master` 创建共享内存）

```bash
# 先启动主站（在另一个终端）
sudo ./build-master/rocos_igh_master --config config/talon_pdo.yaml --master-id 0 --period-us 1000

# 再启动观察器
./tools/shm_web/build/shm_web_server --master-id 0 --port 8484
```

浏览器打开 `http://127.0.0.1:8484`。

**方式二：演示模式**（无需硬件 / 主站进程，自建共享内存并模拟数据）

```bash
./tools/shm_web/build/shm_web_server --demo --port 8484
```

浏览器打开 `http://127.0.0.1:8484`，会看到一组模拟的伺服驱动 PDO 正在动画（位置斜坡、速度正弦等）。

## 命令行参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--master-id N` | `0` | 要观察的 master_id，决定 `/dev/shm/ecm_v2_{N}` 等命名 |
| `--port P` | `8484` | 监听端口 |
| `--host H` | `127.0.0.1` | 绑定地址；设为 `0.0.0.0` 允许局域网访问 |
| `--demo` | 关 | 自建共享内存 + 模拟数据（无硬件也可看效果） |
| `--help` | - | 打印用法 |

## HTTP 端点

| 路径 | 说明 |
|---|---|
| `GET /` | 前端页面（加载可执行文件旁的 `index.html`） |
| `GET /api/snapshot` | 当前共享内存快照（JSON） |
| `GET /api/events` | SSE 流，约每 250 ms 推送一帧快照 |
| `POST /api/output` | 仅演示模式校验目标后单次写入 OUT PDO；真实模式拒绝写入 |

## 显示内容

依据固定 `rocos-shm` 子模块的 ABI v2（构建前运行 `git submodule update --init --recursive`）：

- `EcatBus`：总线状态（IgH 为未知，各从站独立显示 AL 状态）、`dt` 控制周期、周期统计（min/max/avg/current，微秒）、时间戳、`is_authorized`、从站数。
- `Slave`：每个从站的名称、在线/AL 状态及有效性、输入/输出变量数；JSON 同时提供身份和有效位。
- `PdVar`：每个 PDO 变量的名称、`index.sub_index`、字节偏移、大小，以及从 PDO 缓冲区读出的实时字节。数据类型菜单按变量宽度提供 `UINT8`/`INT8`、`UINT16`/`INT16` 或 `UINT32`/`INT32`/`FLOAT`，并以所选类型进行小端解析。

## 数据流

1. 主站进程（`rocos_igh_master`）创建 `/dev/shm/ecm_v2_{id}`、`pd_input_v2_{id}`、`pd_output_v2_{id}` 与 `sem.sync_v2_{id}_*`。
2. `shm_web_server` 用 `connect(true)` 只读客户端路径 `open` 并 `mmap` 这些对象。
3. 前端用 `EventSource` 订阅 `/api/events`，后端每 250 ms 序列化一次快照推送。
4. 观察器通过 `heartbeat_mono_ns` 是否变化来判断主站存活；主站退出后自动进入"等待"重连状态。

真实主站以只读映射连接，OUT 输入框和按钮禁用，不消费信号量或提交命令。
演示模式自建 v2 对象，保留模拟输出写入；owner 锁阻止覆盖同 ID 的活跃主站，
不预先删除任何同名对象。主站或可写 App 崩溃后需停止另一方、关闭旧映射再协调重启，
详见 [ABI v2 迁移说明](../../docs/shared-memory-v2.md)。

### CiA 402 Status Word 标签

自动识别 `0x6041:00`（界面显示 `6041.0`），从原始 2 字节小端数据解析，
在实时值后显示驱动器状态：未准备好上电、禁止上电、准备好上电、已上电、运行已使能、
快速停机中、故障反应中或故障；不匹配标准状态编码时显示“未知状态”。
故障用红色、快速停机用黄色、运行使能用绿色区分，同时保留文字。

仅根据 bit 0～3、bit 5～6 匹配状态，每个状态字只显示一个状态标签。
图中 `x` 位不参与判断：未准备好上电、禁止上电、故障反应中和故障使用 `0x004F` 掩码，
其余四种状态使用 `0x006F` 掩码。忽略 bit 4 和 bit 7～15，不显示附加位标签。
例如 `08 12`（`0x1208`）仅显示“故障”。
标签随快照刷新，切换 UINT16/INT16 不影响状态解析。

状态掩码和位定义参考 [CiA 402 Statusword 设备文档](https://doc.synapticon.com/circulo/sw5.1/objects_html/6xxx/6041.html)。

前端回归测试（需要 Node.js）：

```bash
node --test tools/shm_web/*.test.mjs
```


### 演示模式 OUT 字典写入

OUT 行提供输入框和“写入”按钮（也可在输入框按 Enter）。先选择数据类型，再输入数值：

- 整数支持十进制和 `0x` 十六进制；有符号类型可输入负数。
- `FLOAT` 支持小数和科学计数法；所有类型均检查格式和范围，拒绝越界截断。
- 仅支持 1、2、4 字节的已映射 OUT 字典；IN 和 `0000.0` 填充项没有写入入口。
- 仅演示模式允许写入；真实主站只读，输出由唯一可写 App 控制。点击后将小端字节**写入一次** `pd_output_v2_{master_id}`；不持续锁定数值。
  页面提示的是共享内存写入结果，不代表驱动器已经执行。实时快照用于查看后续值。
- 刷新实时数据不会覆盖正在输入的内容；请求期间禁止重复提交，断线时禁用写入。
- 演示模式第一次写入后停止 OUT 动画，保留输出值，IN 动画继续运行。

后端重新核对主站 ID、从站 ID、OUT 字典索引/子索引、偏移、大小及实际映射边界；
未连接或非演示模式时拒绝写入。真实设备输出由唯一可写 App 管理；Web 工具
始终只读，不提供切换为真实输出写入的选项。

接口请求头要求 `X-Rocos-Write: 1` 和有效的 `Content-Length`，拒绝浏览器跨来源写入。
请求体为以空格分隔的 `master_id slave_id index sub_index offset size hexbytes`，
前六项是非负十进制整数，末项是严格匹配字节长度的小端十六进制数据。
成功返回 `{"ok":true}`；失败返回非 2xx 状态及 `{"error":"原因"}`。

后端无硬件测试（匿名映射及本地 socketpair，不接触真实共享内存）：

```bash
cmake -S tools/shm_web -B tools/shm_web/build
cmake --build tools/shm_web/build
ctest --test-dir tools/shm_web/build --output-on-failure
```
