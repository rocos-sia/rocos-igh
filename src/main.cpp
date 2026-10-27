// =============================================================================
// main.cpp —— rocos_igh_master 主站进程入口
// -----------------------------------------------------------------------------
// 职责：解析命令行参数 → 安装信号处理 → 初始化 EtherCAT 主站与共享内存 IPC
//       → 发布从站配置元数据 → 配置实时调度 → 运行周期任务 → 输出最终统计。
// 本文件是唯一的进程入口，本身不处理 PDO 内容，也不包含从站型号相关逻辑。
// =============================================================================

// 本工程自身头文件：引入各组件接口。
#include "cyclic_task.hpp"          // 绝对时间周期任务（CyclicTask）
#include "ethercat_master.hpp"      // IgH 主站生命周期封装（EthercatMaster）
#include "pdo_config.hpp"           // YAML PDO 配置解析（PdoBusConfig）
#include "runtime_options.hpp"      // 命令行参数解析（RuntimeOptions）
#include "shared_memory_config.hpp" // 共享内存 IPC 与跨进程 ABI（SharedMemoryConfig）
#include "slave_config.hpp"         // 动态 IgH 配置所有权（LoadedSlaveConfig）
#include "thread_affinity.hpp"

// 标准库与系统头文件。
#include <atomic>
#include <chrono>
#include <thread>
#include <array>       // std::array（预触碰栈缓冲区）
#include <cstdlib>     // EXIT_SUCCESS / EXIT_FAILURE
#include <cerrno>      // errno（错误码）
#include <csignal>     // sig_atomic_t / sigaction / SIGINT / SIGTERM
#include <cstring>     // std::strerror
#include <ctime>       // clock_getres / CLOCK_MONOTONIC
#include <iomanip>
#include <sstream>
#include <iostream>    // std::cout / std::cerr
#include <sched.h>     // sched_param / SCHED_FIFO / sched_setscheduler
#include <string>      // std::string
#include <sys/mman.h>  // mlockall / MCL_CURRENT / MCL_FUTURE
#include <utility>     // std::move

namespace {

// 全局退出标志：信号处理函数中唯一允许修改的变量。
// 使用 volatile + sig_atomic_t，保证在信号处理器与主循环之间安全传递。
volatile std::sig_atomic_t g_stop_requested = 0;

// 预触碰栈缓冲区大小（64 KiB）：用于把周期线程即将用到的栈页提前加载到内存。
constexpr std::size_t kPrefaultStackBytes = 64U * 1024U;

// 实时线程优先级（SCHED_FIFO 下的静态优先级 80）。
constexpr int kRealtimePriority = 80;

// 信号处理函数：仅把停止标志置 1，不做任何非异步信号安全的操作。
void handleSignal(int) noexcept {
    g_stop_requested = 1; // 通知主循环退出
}

// 安装 SIGINT/SIGTERM 信号处理器，两者都通过 handleSignal 设置停止标志。
bool installSignalHandlers(std::string &error) {
    error.clear(); // 先清空错误信息，保证失败时返回的是本次错误

    struct sigaction action {};        // 信号动作结构体，零初始化
    action.sa_handler = handleSignal;  // 指定处理函数（不使用 sa_sigaction 形式）
    if (sigemptyset(&action.sa_mask) != 0) { // 清空信号掩码，处理期间不屏蔽其他信号
        error = "sigemptyset failed";  // 记录失败原因
        return false;                  // 掩码初始化失败，直接返回
    }

    if (sigaction(SIGINT, &action, nullptr) != 0) { // 为 Ctrl+C（SIGINT）安装处理器
        error = "sigaction SIGINT failed";
        return false;
    }
    if (sigaction(SIGTERM, &action, nullptr) != 0) { // 为 SIGTERM（服务停止）安装处理器
        error = "sigaction SIGTERM failed";
        return false;
    }

    return true; // 两个信号处理器都安装成功
}

// 进入周期循环前完成实时准备：CPU 绑定、锁定内存、预触碰栈、SCHED_FIFO。
bool configureRealtime(const rocos::RuntimeOptions &options, std::string &error) {
    error.clear();

    // Reporter already exists: only the calling (cyclic) thread is pinned.
    if (options.cpu && !rocos::bindCurrentThreadCpu(*options.cpu, error)) return false;
    if (options.cpu) {
        std::cout << "[Realtime] cyclic thread CPU=" << *options.cpu
                  << " (affinity verified)\n";
    }

    // mlockall 锁定当前及未来所有内存页，避免周期线程运行中发生缺页导致的延迟。
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        error = "mlockall failed: " + std::string(std::strerror(errno));
        return false;
    }

    // 预触碰一段 64 KiB 的栈缓冲区：每隔一页（4096B）写一次，触发缺页并锁定。
    std::array<unsigned char, kPrefaultStackBytes> stack{};
    volatile unsigned char *const stack_data = stack.data(); // volatile 防止写被优化掉
    for (std::size_t i = 0; i < stack.size(); i += 4096U) {
        stack_data[i] = 0; // 触碰每一页，确保已映射到物理内存
    }

    // 设置当前周期线程的 SCHED_FIFO 策略与静态优先级。
    sched_param scheduler{};
    scheduler.sched_priority = kRealtimePriority; // 静态优先级 80
    if (sched_setscheduler(0, SCHED_FIFO, &scheduler) != 0) { // 参数 0 表示当前线程
        error = "sched_setscheduler failed: " + std::string(std::strerror(errno));
        return false;
    }

    return true;
}

// 周期循环结束后输出一行最终统计（非实时路径，允许使用 iostream）。
void printFinalStatistics(const rocos::CycleStatistics &stats, int run_result) {
    std::cout << "final_stats cycles=" << stats.cycles            // 完成的周期数
              << " missed_deadlines=" << stats.missed_deadlines    // 错过的截止期数
              << " min_us=" << stats.minimum_us                    // 最小周期耗时
              << " max_us=" << stats.maximum_us                    // 最大周期耗时
              << " avg_us=" << stats.average_us                    // 平均周期耗时
              << " current_us=" << stats.current_us                // 最近一周期耗时
              << " run_rc=" << run_result << '\n';                 // 运行返回码
}

// Only the ordinary-priority reporter formats or writes periodic diagnostics.
void printRuntimeReport(unsigned int master_id, std::size_t slave_count,
                        const rocos::RuntimeReport &report) {
    const auto wcName = [](unsigned int state) {
        switch (state) {
        case EC_WC_ZERO: return "ZERO";
        case EC_WC_INCOMPLETE: return "INCOMPLETE";
        case EC_WC_COMPLETE: return "COMPLETE";
        default: return "UNKNOWN";
        }
    };
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << "[master " << master_id << "] window=" << report.window_seconds
        << "s cycles=" << report.cycle.samples
        << " link=" << (report.link_up ? "UP" : "DOWN")
        << " slaves=" << report.responding_slaves << '/' << slave_count << " AL=";
    // IgH reports a bitmask: mixed states must not be rendered as a single OP.
    if (report.al_states == 0) out << "NONE";
    if (report.al_states & EC_AL_STATE_INIT) out << "INIT ";
    if (report.al_states & EC_AL_STATE_PREOP) out << "PREOP ";
    if (report.al_states & EC_AL_STATE_SAFEOP) out << "SAFEOP ";
    if (report.al_states & EC_AL_STATE_OP) out << "OP ";
    out << "(0x" << std::hex << report.al_states << std::dec << ")"
        << " authorized=" << report.authorized
        << " input_wc=" << report.input_wc << '/' << wcName(report.input_wc_state)
        << " output_wc=" << report.output_wc << '/' << wcName(report.output_wc_state) << '\n';
    const auto timing = [&](const char *name, const rocos::TimingSummary &t) {
        out << "  " << std::left << std::setw(22) << name
            << " (min/avg/max) [us]: " << std::right
            << t.minimum_us << " / " << t.average_us << " / " << t.maximum_us << '\n';
    };
    timing("Cycle Time", report.cycle);
    timing("Task Duration", report.task);
    timing("Receive / Process", report.receive);
    timing("PDO Copy", report.pdo);
    timing("Queue / Send", report.send);
    out << "  window/total: missed_deadlines=" << report.missed << '/' << report.total_missed
        << " wc_error_cycles=" << report.wc_errors << '/' << report.total_wc_errors
        << " link_down_cycles=" << report.link_down << '/' << report.total_link_down << '\n';
    std::cout << out.str() << std::flush;
}

}  // namespace（匿名命名空间，仅本文件可见）

int main(int argc, char **argv) {
    // —— 第一步：解析命令行参数 ——
    rocos::RuntimeOptions options{}; // 参数解析结果（默认 master_id=0，period_us=1000）
    std::string error;               // 复用同一错误字符串，供各阶段写入失败原因
    if (!rocos::parseRuntimeOptions(argc, argv, options, error)) { // 解析失败（非法选项/取值）
        std::cerr << error << '\n' << rocos::runtimeOptionsUsage() << '\n'; // 打印错误与用法
        return EXIT_FAILURE; // 参数错误，直接退出
    }
    if (options.show_help) { // 用户请求了 --help
        std::cout << rocos::runtimeOptionsUsage() << '\n'; // 只打印用法
        return EXIT_SUCCESS; // 正常退出
    }

    // Load PDOs and startup defaults once, then overlay only explicit CLI flags.
    rocos::PdoBusConfig pdo_config;
    if (!rocos::loadPdoConfig(options.config_path, pdo_config, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    if (!rocos::parseRuntimeOptions(argc, argv, options, error, pdo_config.master)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    // Validate before reserving the master or changing any slave configuration.
    if (options.cpu && !rocos::validateThreadCpu(*options.cpu, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    timespec resolution{};
    if (clock_getres(CLOCK_MONOTONIC, &resolution) != 0) {
        std::cerr << "clock_getres(CLOCK_MONOTONIC) failed: " << std::strerror(errno) << '\n';
        return EXIT_FAILURE;
    }
    if (resolution.tv_sec != 0 || resolution.tv_nsec != 1) {
        std::cerr << "[Realtime] WARNING: CLOCK_MONOTONIC resolution="
                  << resolution.tv_sec << " s + " << resolution.tv_nsec
                  << " ns; expected 1 ns high-resolution clock\n";
    }

    // Owns the PDO views, including entries discovered during PREOP initialization.
    rocos::LoadedSlaveConfig loaded_config;

    // —— 第三步：安装信号处理器 ——
    if (!installSignalHandlers(error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    // —— 第四步：初始化 EtherCAT 主站（请求主站、建 domain、配从站、激活）——
    rocos::EthercatMaster master; // RAII 封装一个 IgH 主站
    if (!master.initialize(options, std::move(pdo_config), loaded_config, error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    rocos::StaticSlaveConfig config = loaded_config.view();
    rocos::printLoadedSlaveConfig(loaded_config, std::cout);

    // —— 第五步：创建共享内存 IPC（总线快照 + PDO 缓冲区）——
    rocos::SharedMemoryConfig ipc(static_cast<int>(options.master_id)); // 按 master_id 隔离命名
    if (!ipc.createSharedMemory()) { // 创建 ABI v2 总线共享内存与信号量
        std::cerr << "failed to create shared memory" << '\n';
        return EXIT_FAILURE;
    }
    if (!ipc.createPdDataMemoryProvider(static_cast<int>(master.sharedInputSize()),
                                        static_cast<int>(master.sharedOutputSize()))) {
        std::cerr << "failed to create process data memory" << '\n';
        return EXIT_FAILURE;
    }

    // —— 第六步：把从站/PDO 元数据发布到共享内存，供客户端读取 ——
    if (!rocos::publishConfig(*ipc.ecatBus, config, error)) { // 填充 EcatBus 元数据
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    if (!master.publishIdentity(*ipc.ecatBus)) {
        std::cerr << "failed to publish slave identity" << '\n';
        return EXIT_FAILURE;
    }
    static_assert(sizeof(ROCOS_IGH_VERSION) <= sizeof(ipc.ecatBus->ecm_version));
    std::strcpy(ipc.ecatBus->ecm_version, ROCOS_IGH_VERSION);
    ipc.ecatBus->backend_type = 2U;
    ipc.ecatBus->supported_cmds = 1U << rocos::CMD_RESET_STATS;
    ipc.ecatBus->state_control_supported = 0U;
    ipc.ecatBus->dt = options.period_us;
    ipc.ecatBus->current_state = 0; // IgH al_states is a set, not one master state.
    ipc.ecatBus->is_authorized = true;
    ipc.ecatBus->dc_sync_status = options.dc_enabled ? 4U : 0U;
    // DC deviation/lock and firmware are unavailable: leave their validity unset.

    // Start the reporter before SCHED_FIFO/mlockall; only this normal-priority
    // thread formats startup progress. The cyclic path publishes lock-free data.
    rocos::CyclicTask cyclic_task(master, ipc, options.period_us, options.op_timeout_ms);
    std::atomic<bool> reporter_stop{false};
    std::thread reporter;
    try {
        reporter = std::thread([&] {
            auto next_report = std::chrono::steady_clock::now();
            bool ready_announced = false;
            for (;;) {
                const bool stopped = reporter_stop.load();
                if (!stopped) {
                    ipc.processCommand([&](const rocos::AsyncCommandBox &request,
                                           rocos::AsyncCommandBox &) -> std::int32_t {
                        if (request.cmd_type != rocos::CMD_RESET_STATS) return rocos::UNSUPPORTED;
                        if (!cyclic_task.startupReady()) return rocos::STATE_MISMATCH;
                        return cyclic_task.statisticsReset().execute(request.deadline_mono_ns, reporter_stop);
                    });
                }
                const bool ready = cyclic_task.startupReady();
                if (ready && !ready_announced) {
                    std::cout << "[master " << options.master_id
                              << "] 全部从站就绪，可以开始工作（全部 RxPDO/TxPDO 分配及映射读回一致，OP、链路及输入/输出工作计数器连续 5 次检查正常）"
                              << std::endl;
                    ready_announced = true;
                }
                rocos::RuntimeReport report;
                if (cyclic_task.takeReport(report)) {
                    printRuntimeReport(options.master_id, config.slave_count, report);
                }
                if (stopped) return;
                const auto now = std::chrono::steady_clock::now();
                const auto mask = cyclic_task.startupOpMask();
                if (!ready && now >= next_report) {
                    std::cout << "[master " << options.master_id << "] 等待 OP / PDO 读回验证，当前已进入 OP 的从站：";
                    std::size_t count = 0;
                    for (std::size_t i = 0; i < config.slave_count; ++i) {
                        if ((mask & (std::uint64_t{1} << i)) == 0) continue;
                        std::cout << " [id=" << config.slaves[i].position
                                  << ", name=" << config.slaves[i].name << "]";
                        ++count;
                    }
                    if (count == 0) std::cout << " 无";
                    std::cout << " (" << count << '/' << config.slave_count << ")" << std::endl;
                    next_report = now + std::chrono::seconds(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    } catch (const std::exception &exception) {
        std::cerr << "failed to start OP reporter: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    // Only expose the initialized ABI after the non-RT command worker exists.
    if (!ipc.publishReady()) {
        reporter_stop.store(true);
        reporter.join();
        std::cerr << "failed to publish shared memory: " << ipc.lastError() << '\n';
        return EXIT_FAILURE;
    }

    // —— 第七步：配置实时环境（锁内存、预触碰、SCHED_FIFO）——
    if (!configureRealtime(options, error)) {
        rocos::atomicStore(ipc.header->ready, 0U);
        reporter_stop.store(true);
        reporter.join();
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }

    // —— 第八步：运行周期任务直到收到停止信号 ——
    const int run_result = cyclic_task.run(g_stop_requested);      // 阻塞运行，直到停止标志置位
    rocos::atomicStore(ipc.header->ready, 0U);
    reporter_stop.store(true);
    reporter.join();
    printFinalStatistics(cyclic_task.statistics(), run_result);    // 循环结束后输出统计

    if (run_result != 0) { // 周期任务以错误码结束（如时钟调用失败）
        master.pdoVerification().printError(std::cerr);
        std::cerr << "cyclic task failed: " << std::strerror(run_result) << '\n';
        const rocos::DcError dc_error = master.lastDcError();
        if (dc_error.stage != rocos::DcErrorStage::None) {
            const int positive_error = dc_error.error_code < 0
                                           ? -dc_error.error_code
                                           : dc_error.error_code;
            std::cerr << "DC operation failed: "
                      << rocos::dcErrorStageName(dc_error.stage)
                      << " rc=" << dc_error.error_code
                      << " (" << std::strerror(positive_error) << ")\n";
        }
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS; // 正常退出；局部对象按逆序析构，释放 IgH 与 IPC 资源
}
