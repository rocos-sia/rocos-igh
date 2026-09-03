// =============================================================================
// shm_web_server.cpp —— rocos_igh 共享内存观察器（Web 后端）
// -----------------------------------------------------------------------------
// 一个零依赖的静态 HTTP 服务器，复用 SharedMemoryConfig 的客户端路径连接到
// 主站进程创建的 POSIX 共享内存（ecm{id} / pd_input{id} / pd_output{id}），
// 把总线快照与 PDO 变量值以 JSON 形式暴露给浏览器前端，并用 Server-Sent
// Events 周期性推送，实现"动态显示"。
//
// 用法：
//   shm_web_server [--master-id N] [--port P] [--host H] [--demo]
//     --master-id  要观察的 master_id（默认 0）
//     --port       监听端口（默认 8484）
//     --host       绑定地址（默认 127.0.0.1，仅本机可访问）
//     --demo       自建一份共享内存并模拟数据（无硬件/主站也能看效果）
//
// 端点：
//   GET /              前端页面（index.html，从可执行文件旁加载）
//   GET /api/snapshot  当前共享内存快照（JSON）
//   GET /api/events    SSE 流，周期推送快照
// =============================================================================

#include "shared_memory_config.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using rocos::EcatBus;
using rocos::PdVar;
using rocos::Slave;
using rocos::SharedMemoryConfig;

// -----------------------------------------------------------------------------
// 常量
// -----------------------------------------------------------------------------
constexpr int kDefaultPort    = 8484;
constexpr int kPushIntervalMs = 250;   // SSE 推送间隔
constexpr int kReconnectMs    = 1000;  // 未连接时的重试间隔
constexpr int kStaleMs        = 3000;  // 判定主站已退出的时间阈值
constexpr int kStaleUs        = kStaleMs * 1000;

// -----------------------------------------------------------------------------
// 应用状态：被所有连接线程共享
// -----------------------------------------------------------------------------
struct AppState {
    int master_id = 0;
    bool demo     = false;

    std::mutex mtx;
    std::unique_ptr<SharedMemoryConfig> cfg;  // 客户端连接（或 demo 模式下自建）
    bool connected = false;

    std::chrono::steady_clock::time_point last_attempt{};  // 上次尝试连接时刻
    long last_ts     = 0;                                  // 上一次观察到的时间戳
    std::chrono::steady_clock::time_point last_seen_ts{};  // 上次看到时间戳更新的时刻
};

// -----------------------------------------------------------------------------
// 基础工具
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// 全局退出标志：信号处理函数置位，主循环据此关闭监听并退出。
// -----------------------------------------------------------------------------
volatile std::sig_atomic_t g_shutdown = 0;
void handleSignal(int) { g_shutdown = 1; }

void ignoreSigpipe() {
    std::signal(SIGPIPE, SIG_IGN);
}

// 安装 SIGINT/SIGTERM 处理器（用于干净退出、清理 demo 共享内存）。
// 用 sigaction 且不设置 SA_RESTART，保证 accept() 能被打断返回 EINTR，
// 从而主循环能观察到 g_shutdown 并退出。
void installSignalHandlers() {
    struct sigaction sa {};
    sa.sa_handler = handleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // 显式不带 SA_RESTART
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// JSON 字符串转义（名称来自共享内存的 char 数组）。
std::string jsonEscape(const std::string &s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b";  break;
            case '\f': out << "\\f";  break;
            case '\n': out << "\\n";  break;
            case '\r': out << "\\r";  break;
            case '\t': out << "\\t";  break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    return out.str();
}

std::string cstr(const char *p, std::size_t cap) {
    return std::string(p, strnlen(p, cap));
}

const char *stateName(int state) {
    switch (state) {
        case 1: return "INIT";
        case 2: return "PREOP";
        case 3: return "BOOTSTRAP";
        case 4: return "SAFEOP";
        case 8: return "OP";
        default: return "?";
    }
}

// 读取并十六进制编码一段 PDO 数据。
std::string hexBytes(const void *base, int offset, int size) {
    std::ostringstream out;
    if (base == nullptr || offset < 0 || size <= 0) return "";
    const auto *p = static_cast<const unsigned char *>(base) + offset;
    for (int i = 0; i < size; ++i) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(p[i]);
    }
    return out.str();
}

// -----------------------------------------------------------------------------
// demo 模式：自建共享内存并模拟一个 EtherCAT 驱动
// -----------------------------------------------------------------------------
// PDO 布局（与 src/slave_config.cpp 的 drive 从站一致）：
//   输出 13 B:  TargetPosition[4] TargetVelocity[4] TargetTorque[2]
//               ControlWord[2] ModesOfOperation[1]
//   输入 18 B:  StatusWord[2] Position[4] Velocity[4] Torque[2]
//               AuxPosition[4] AnalogInput[2]
struct DemoVar {
    const char *name;
    int  offset;
    int  size;
    int  index;
    int  sub_index;
};

constexpr DemoVar kDemoOutputVars[] = {
    {"Target Position",       0,  4, 0x607A, 0x00},
    {"Target Velocity",       4,  4, 0x60FF, 0x00},
    {"Target Torque",         8,  2, 0x6071, 0x00},
    {"Control Word",         10,  2, 0x6040, 0x00},
    {"Modes of Operation",   12,  1, 0x6060, 0x00},
};
constexpr DemoVar kDemoInputVars[] = {
    {"Status Word",              0,  2, 0x6041, 0x00},
    {"Position Actual Value",    2,  4, 0x6064, 0x00},
    {"Velocity Actual Value",    6,  4, 0x606C, 0x00},
    {"Torque Actual Value",     10,  2, 0x6077, 0x00},
    {"Auxiliary Position Value",12,  4, 0x20A0, 0x00},
    {"Analog Input",            16,  2, 0x2205, 0x02},
};

void populateDemoBus(EcatBus &bus) {
    bus = EcatBus{};
    bus.dt             = 1000;
    bus.current_state  = ECAT_STATE_OP;
    bus.request_state  = ECAT_STATE_OP;
    bus.next_expected_state = ECAT_STATE_OP;
    bus.is_authorized  = true;
    bus.slave_num      = 1;

    Slave &slave = bus.slaves[0];
    slave.id     = 0;
    std::strncpy(slave.name, "EtherCAT Drive (demo)", MAX_SLAVE_NAME_LEN - 1);
    slave.name[MAX_SLAVE_NAME_LEN - 1] = '\0';

    auto copyVar = [](PdVar &dst, const DemoVar &v) {
        std::strncpy(dst.name, v.name, MAX_PD_NAME_LEN - 1);
        dst.name[MAX_PD_NAME_LEN - 1] = '\0';
        dst.offset    = v.offset;
        dst.size      = v.size;
        dst.index     = v.index;
        dst.sub_index = v.sub_index;
    };

    int out_count = 0, in_count = 0;
    for (const auto &v : kDemoOutputVars) copyVar(slave.output_vars[out_count++], v);
    for (const auto &v : kDemoInputVars)  copyVar(slave.input_vars[in_count++], v);
    slave.output_var_num = out_count;
    slave.input_var_num  = in_count;
}

// 清理指定 master_id 可能残留的共享内存/信号量（demo 启动前调用，幂等）。
void cleanupShmResiduals(int id) {
    char name[64];
    std::snprintf(name, sizeof name, "/ecm%d", id);      shm_unlink(name);
    std::snprintf(name, sizeof name, "/pd_input%d", id); shm_unlink(name);
    std::snprintf(name, sizeof name, "/pd_output%d", id); shm_unlink(name);
    for (int i = 0; i < EC_SEM_NUM; ++i) {
        std::snprintf(name, sizeof name, "/sync%d_%d", id, i);
        sem_unlink(name);
    }
}

bool createDemoMemory(AppState &st, std::string &why) {
    cleanupShmResiduals(st.master_id);  // 清理上次异常退出的残留
    try {
        st.cfg = std::make_unique<SharedMemoryConfig>(st.master_id);
    } catch (const std::exception &) {
        why = "invalid master id";
        return false;
    }
    if (!st.cfg->createSharedMemory()) { why = "createSharedMemory failed"; return false; }
    if (!st.cfg->createPdDataMemoryProvider(18, 13)) { why = "createPdDataMemoryProvider failed"; return false; }
    populateDemoBus(*st.cfg->ecatBus);
    return true;
}

// 每一帧修改模拟的输入/输出 PDO（位置斜坡、速度正弦等做动画）。
void animateDemo(AppState &st) {
    EcatBus *bus = st.cfg->ecatBus;
    static long n = 0;
    ++n;

    // 模拟主站运行：时间戳递增 + 周期统计。
    bus->timestamp = n;
    bus->current_cycle_time = 1000.0;
    bus->min_cycle_time = 980.0;
    bus->max_cycle_time = 1120.0;
    bus->avg_cycle_time = 1002.0;

    auto *in  = static_cast<unsigned char *>(st.cfg->pdInputPtr);
    auto *out = static_cast<unsigned char *>(st.cfg->pdOutputPtr);

    auto put32le = [](unsigned char *base, int off, int32_t v) {
        base[off]     = static_cast<unsigned char>(v & 0xff);
        base[off + 1] = static_cast<unsigned char>((v >> 8) & 0xff);
        base[off + 2] = static_cast<unsigned char>((v >> 16) & 0xff);
        base[off + 3] = static_cast<unsigned char>((v >> 24) & 0xff);
    };
    auto put16le = [](unsigned char *base, int off, int16_t v) {
        base[off]     = static_cast<unsigned char>(v & 0xff);
        base[off + 1] = static_cast<unsigned char>((v >> 8) & 0xff);
    };
    auto put8 = [](unsigned char *base, int off, int8_t v) {
        base[off] = static_cast<unsigned char>(v);
    };

    int32_t pos = static_cast<int32_t>(n * 100);
    int32_t vel = static_cast<int32_t>(2000 * std::sin(double(n) / 20));

    // 输入：
    put16le(in,  0, 0x0237);             // Status Word
    put32le(in,  2, pos);                // Position Actual Value
    put32le(in,  6, vel);                // Velocity Actual Value
    put16le(in, 10, 1000);               // Torque Actual Value
    put32le(in, 12, pos / 10);           // Auxiliary Position Value
    put16le(in, 16, static_cast<int16_t>(n % 1000));  // Analog Input

    // 输出（真实场景由客户端写入，这里写出跟随值演示回读）：
    put32le(out,  0, pos);               // Target Position
    put32le(out,  4, vel);               // Target Velocity
    put16le(out,  8, 750);               // Target Torque
    put16le(out, 10, 0x0006);            // Control Word
    put8(out,    12, 8);                 // Modes of Operation (csp)
}

// -----------------------------------------------------------------------------
// 快照渲染：把 EcatBus + PDO 值序列化为 JSON
// -----------------------------------------------------------------------------
std::string renderSnapshot(AppState &st) {
    std::lock_guard<std::mutex> lk(st.mtx);
    const auto now = std::chrono::steady_clock::now();

    if (!st.cfg) {
        try {
            st.cfg = std::make_unique<SharedMemoryConfig>(st.master_id);
        } catch (...) {
            st.cfg.reset();
        }
    }

    if (!st.connected) {
        const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(now - st.last_attempt);
        if (st.demo) {
            st.connected = true;
        } else if (since_last.count() >= kReconnectMs) {
            st.last_attempt = now;
            if (st.cfg && st.cfg->getSharedMemory() && st.cfg->getPdDataMemoryProvider()) {
                st.connected = true;
            }
        }
    } else if (!st.demo && st.cfg && st.cfg->ecatBus != nullptr) {
        // 存活检测：时间戳长时间没变，判定主站已退出，丢弃映射以便重连。
        if (st.cfg->ecatBus->timestamp != st.last_ts) {
            st.last_ts = st.cfg->ecatBus->timestamp;
            st.last_seen_ts = now;
        } else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - st.last_seen_ts).count() > kStaleMs) {
            st.cfg.reset();
            st.connected = false;
        }
    }

    std::ostringstream o;
    if (!st.connected || !st.cfg || st.cfg->ecatBus == nullptr) {
        o << "{\"master_id\":" << st.master_id
          << ",\"available\":false,"
          << "\"demo\":" << (st.demo ? "true" : "false")
          << ",\"error\":\"no shared memory for master " << st.master_id << "\"}";
        return o.str();
    }

    const EcatBus *bus = st.cfg->ecatBus;
    (void)kStaleUs;  // 保留常量占位（阈值已用毫秒比较）。
    o << "{"
      << "\"master_id\":" << st.master_id << ","
      << "\"available\":true,"
      << "\"demo\":" << (st.demo ? "true" : "false") << ","
      << "\"timestamp\":" << bus->timestamp << ","
      << "\"dt\":" << bus->dt << ","
      << "\"current_state\":" << bus->current_state << ","
      << "\"current_state_name\":\"" << stateName(bus->current_state) << "\","
      << "\"request_state\":" << bus->request_state << ","
      << "\"next_expected_state\":" << bus->next_expected_state << ","
      << "\"is_authorized\":" << (bus->is_authorized ? "true" : "false") << ","
      << "\"resetCycleTime\":" << (bus->resetCycleTime ? "true" : "false") << ","
      << "\"cycle\":{\"min\":" << bus->min_cycle_time
      << ",\"max\":" << bus->max_cycle_time
      << ",\"avg\":" << bus->avg_cycle_time
      << ",\"current\":" << bus->current_cycle_time << "},"
      << "\"slave_num\":" << bus->slave_num << ","
      << "\"slaves\":[";

    for (int s = 0; s < static_cast<int>(bus->slave_num) && s < MAX_SLAVE_NUM; ++s) {
        const Slave &slave = bus->slaves[s];
        if (s != 0) o << ",";
        o << "{\"id\":" << slave.id
          << ",\"name\":\"" << jsonEscape(cstr(slave.name, MAX_SLAVE_NAME_LEN)) << "\""
          << ",\"input_var_num\":" << slave.input_var_num
          << ",\"output_var_num\":" << slave.output_var_num
          << ",\"input_vars\":[";

        for (int i = 0; i < static_cast<int>(slave.input_var_num) && i < MAX_PDINPUT_NUM; ++i) {
            const PdVar &pv = slave.input_vars[i];
            if (i != 0) o << ",";
            o << "{\"name\":\"" << jsonEscape(cstr(pv.name, MAX_PD_NAME_LEN)) << "\""
              << ",\"offset\":" << pv.offset
              << ",\"size\":" << pv.size
              << ",\"index\":\"" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << pv.index << std::dec << "\""
              << ",\"sub_index\":" << static_cast<int>(pv.sub_index)
              << ",\"bytes\":\"" << hexBytes(st.cfg->pdInputPtr, pv.offset, pv.size) << "\"}";
        }
        o << "],\"output_vars\":[";

        for (int i = 0; i < static_cast<int>(slave.output_var_num) && i < MAX_PDOUTPUT_NUM; ++i) {
            const PdVar &pv = slave.output_vars[i];
            if (i != 0) o << ",";
            o << "{\"name\":\"" << jsonEscape(cstr(pv.name, MAX_PD_NAME_LEN)) << "\""
              << ",\"offset\":" << pv.offset
              << ",\"size\":" << pv.size
              << ",\"index\":\"" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << pv.index << std::dec << "\""
              << ",\"sub_index\":" << static_cast<int>(pv.sub_index)
              << ",\"bytes\":\"" << hexBytes(st.cfg->pdOutputPtr, pv.offset, pv.size) << "\"}";
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

// -----------------------------------------------------------------------------
// 极简 HTTP 响应
// -----------------------------------------------------------------------------
// keep_alive=true 时不发送 "Connection: close"（SSE 长连接需要）。
// body 为空且 keep_alive 时也省略 Content-Length（SSE 是流式、无固定长度）。
std::string response(int status, const std::string &status_text,
                     const std::string &ctype, const std::string &body,
                     bool keep_alive = false) {
    std::ostringstream o;
    o << "HTTP/1.1 " << status << " " << status_text << "\r\n"
      << "Content-Type: " << ctype << "\r\n";
    if (!(body.empty() && keep_alive)) {
        o << "Content-Length: " << body.size() << "\r\n";
    }
    o << "Cache-Control: no-cache\r\n"
      << "Access-Control-Allow-Origin: *\r\n";
    if (keep_alive) {
        o << "Connection: keep-alive\r\n"
          << "X-Accel-Buffering: no\r\n";
    } else {
        o << "Connection: close\r\n";
    }
    o << "\r\n" << body;
    return o.str();
}

bool loadIndexHtml(std::string &out, const std::string &argv0) {
    std::string dir;
    const auto slash = argv0.find_last_of('/');
    dir = (slash == std::string::npos) ? std::string(".") : argv0.substr(0, slash);
    const std::vector<std::string> candidates = { dir + "/index.html", "./index.html", "index.html" };
    for (const auto &path : candidates) {
        std::ifstream f(path, std::ios::binary);
        if (f.good()) {
            std::ostringstream ss;
            ss << f.rdbuf();
            out = ss.str();
            return true;
        }
    }
    return false;
}

bool parseRequest(const std::string &raw, std::string &method, std::string &target) {
    const auto line_end = raw.find("\r\n");
    if (line_end == std::string::npos) return false;
    std::istringstream line(raw.substr(0, line_end));
    line >> method >> target;
    return method == "GET";
}

void handleConnection(int fd, AppState &st, const std::string &argv0) {
    std::string raw;
    raw.reserve(4096);
    char buf[4096];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { close(fd); return; }
        raw.append(buf, n);
        if (raw.size() > 8192) { close(fd); return; }
    }

    std::string method, target;
    if (!parseRequest(raw, method, target)) {
        const std::string r = response(405, "Method Not Allowed", "text/plain; charset=utf-8", "method not allowed\n");
        ::send(fd, r.data(), r.size(), MSG_NOSIGNAL);
        close(fd);
        return;
    }

    const auto path = target.substr(0, target.find('?'));

    if (path == "/" || path == "/index.html") {
        std::string html;
        if (loadIndexHtml(html, argv0)) {
            const std::string r = response(200, "OK", "text/html; charset=utf-8", html);
            ::send(fd, r.data(), r.size(), MSG_NOSIGNAL);
        } else {
            const std::string r = response(200, "OK", "text/html; charset=utf-8",
                "<h1>index.html not found</h1><p>Place index.html next to the executable or in the current directory.</p>");
            ::send(fd, r.data(), r.size(), MSG_NOSIGNAL);
        }
        close(fd);
    } else if (path == "/api/snapshot") {
        const std::string r = response(200, "OK", "application/json; charset=utf-8", renderSnapshot(st));
        ::send(fd, r.data(), r.size(), MSG_NOSIGNAL);
        close(fd);
    } else if (path == "/api/events") {
        // SSE 长连接：先发响应头（keep-alive、无 Content-Length），再周期推送。
        const std::string head = response(200, "OK", "text/event-stream; charset=utf-8", "", true);
        ::send(fd, head.data(), head.size(), MSG_NOSIGNAL);
        // 立即推一帧，建立首屏；隔一段时间判断连接是否仍有效（客户端断开时 send 返回 -1）。
        bool alive = true;
        while (alive) {
            const std::string snap = renderSnapshot(st);
            const std::string ev = "data: " + snap + "\n\n";
            const ssize_t n = ::send(fd, ev.data(), ev.size(), MSG_NOSIGNAL);
            if (n < 0) { alive = false; break; }  // EPIPE/ECONNRESET：客户端已断开
            std::this_thread::sleep_for(std::chrono::milliseconds(kPushIntervalMs));
        }
        close(fd);
    } else {
        const std::string r = response(404, "Not Found", "text/plain; charset=utf-8", "not found\n");
        ::send(fd, r.data(), r.size(), MSG_NOSIGNAL);
        close(fd);
    }
}

}  // namespace

int main(int argc, char **argv) {
    AppState st;

    int port = kDefaultPort;
    std::string host = "127.0.0.1";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--master-id") == 0 && i + 1 < argc) {
            st.master_id = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (std::strcmp(argv[i], "--demo") == 0) {
            st.demo = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "shm_web_server [--master-id N] [--port P] [--host H] [--demo]\n";
            return 0;
        }
    }

    if (st.demo) {
        std::string why;
        if (!createDemoMemory(st, why)) {
            std::cerr << "demo setup failed: " << why << "\n";
            return 1;
        }
        st.connected = true;
    }
    ignoreSigpipe();
    installSignalHandlers();

    const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { std::perror("socket"); return 1; }
    const int on = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::perror("bind");
        std::cerr << "failed to bind " << host << ":" << port << "\n";
        return 1;
    }
    if (listen(listen_fd, 16) != 0) { std::perror("listen"); return 1; }

    if (st.demo) {
        std::thread([&st]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kPushIntervalMs));
                std::lock_guard<std::mutex> lk(st.mtx);
                if (st.cfg) animateDemo(st);
            }
        }).detach();
    }

    std::cout << "[shm_web] listening on http://" << host << ":" << port
              << "  (master_id=" << st.master_id << (st.demo ? ", demo mode" : "") << ")\n";
    std::cout << "[shm_web] open this URL in a browser\n";

    for (;;) {
        const int client = accept(listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR || g_shutdown) break;
            continue;
        }
        std::thread(handleConnection, client, std::ref(st), std::string(argv[0])).detach();
    }
    close(listen_fd);

    // 干净退出时清理 demo 自建的共享内存（st 在析构时 unlink owns 的对象）。
    st.cfg.reset();
    return 0;
}
