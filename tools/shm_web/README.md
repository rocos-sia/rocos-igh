# shm_web —— rocos_igh 共享内存实时观察器（Web）

一个零依赖的小工具：**C++17 后端**（读懂 POSIX 共享内存）+ **纯 HTML/JS 前端**（浏览器实时展示）。
复用 [src/shared_memory_config.hpp](../../src/shared_memory_config.hpp) 的客户端路径连接到主站进程创建的共享内存，
把总线快照与 PDO 变量值以 JSON 暴露，并用 Server-Sent Events 周期推送，实现"动态显示"。

## 目录

- `shm_web_server.cpp` —— HTTP 后端：静态文件 + `/api/snapshot`（JSON 快照）+ `/api/events`（SSE 推送）。
- `index.html` —— 前端单页：总线状态卡片、周期统计、每个从站的输入/输出 PDO 变量实时值。
- `value_decoder.mjs` —— 小端 PDO 数据类型解析，供浏览器页面与 Node.js 测试复用。
- `CMakeLists.txt` —— 构建脚本（独立于主工程，可单独编译）。

## 构建

```bash
cmake -S tools/shm_web -B tools/shm_web/build
cmake --build tools/shm_web/build
```

生成 `tools/shm_web/build/shm_web_server`，并把 `index.html` 复制到其旁。

## 用法

**方式一：观察真实主站**（需要先运行 `rocos_igh_master` 创建共享内存）

```bash
# 先启动主站（在另一个终端）
sudo ./build-master/rocos_igh_master --master-id 0 --period-us 1000

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
| `--master-id N` | `0` | 要观察的 master_id，决定 `/dev/shm/ecm{N}` 等命名 |
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

## 显示内容

依据 `shared_memory_config.hpp` 的 ABI：

- `EcatBus`：总线状态（INIT/PREOP/SAFEOP/OP）、`dt` 控制周期、周期统计（min/max/avg/current，微秒）、时间戳、`is_authorized`、从站数。
- `Slave`：每个从站的名称、输入/输出变量数。
- `PdVar`：每个 PDO 变量的名称、`index.sub_index`、字节偏移、大小，以及从 PDO 缓冲区读出的实时字节。数据类型菜单按变量宽度提供 `UINT8`/`INT8`、`UINT16`/`INT16` 或 `UINT32`/`INT32`/`FLOAT`，并以所选类型进行小端解析。

## 数据流

1. 主站进程（`rocos_igh_master`）创建 `/dev/shm/ecm{id}`、`pd_input{id}`、`pd_output{id}` 与 `sem.sync{id}_*`。
2. `shm_web_server` 用 `SharedMemoryConfig::getInstance` 客户端路径 `open` 并 `mmap` 这些对象。
3. 前端用 `EventSource` 订阅 `/api/events`，后端每 250 ms 序列化一次快照推送。
4. 观察器通过 `ecm{id}->timestamp` 是否变化来判断主站存活；主站退出后自动进入"等待"重连状态。

> **注意**：观察器以只读客户端身份连接，不会创建/清理共享内存（`--demo` 模式除外——它自建并负责清理）。
> 主站进程的共享内存由其自身在退出时 unlink；若主站被 `SIGKILL` 强杀，可能残留，需手动清理对应 `/dev/shm/` 对象。
