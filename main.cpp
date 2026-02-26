#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <iostream>
#include <atomic>
#include <memory>
#include <thread>
#include <string>
#include <vector>
#include <cstdint>
#include <clocale>
#include <windows.h>
#include "src/CheckpointStore.h"
#include "src/TencentBot.h"
#include "src/DxgiWindowCapture.h"
#include "src/CaptchaEngine.h"
#include "src/MemoryReader.h"
#include "src/SharedDataStatus.h"
#include "src/runtime/RuntimeController.h"

TencentBot bot;
// 全局停止标志：
// - 控制台 Ctrl+C 会置位
// - 远程 STOP 指令会置位
// - 业务线程在关键循环中轮询该标志并安全退出
static std::atomic_bool g_stopRequested{false};

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stopRequested.store(true, std::memory_order_relaxed);
            // 已处理，交由业务层走“安全收尾 + checkpoint 保存”流程。
            return TRUE;
        default:
            return FALSE;
    }
}

namespace {
    // 启动参数帮助：
    // 既覆盖 checkpoint 维护，也覆盖调试模式和远程控制模式。
    void printCheckpointHelp() {
        std::cout <<
R"(Checkpoint tools:
  --checkpoint <path>         checkpoint 文件路径（默认 bot_checkpoint.json）
  --show-checkpoint           打印当前 checkpoint 并退出
  --reset-checkpoint          删除 checkpoint（下次从头开始）并退出
  --set-next-op <n>           设置 next_op 并退出
  --set-cycle <n>             设置 cycle 并退出
  --set-target-money <n>      设置目标银票（默认 150000）并退出
  --print-cursor              持续打印当前鼠标坐标（调试模式）
  --cursor-interval-ms <n>    鼠标坐标打印间隔毫秒（默认 100）
  --pid <n>                   print-cursor 模式下用于读内存链的目标 PID
  --remote-control            开启远程控制模式（通过 TCP 指令启停）
  --remote-port <n>           远程控制监听端口（默认 19090）

next_op 对应关系（v3）:
  0: leave_gang_to_difu        出帮派 -> 长安 -> 大唐国境 -> 地府
  1: difu_buy_paper            地府买纸钱
  2: travel_to_beiju           地府 -> ... -> 北俱
  3: beiju_sell_paper_buy_oil  北俱卖纸钱 + 买油（卖完会立即判断是否回帮派）
  4: return_to_difu            北俱 -> ... -> 地府
  5: difu_sell_oil             地府卖油（卖完会立即判断是否回帮派）
)";
    }

    // 通用整数解析（十进制），用于 checkpoint 相关参数。
    bool parseInt(const std::string& s, int& out) {
        try { out = std::stoi(s); return true; } catch (...) { return false; }
    }

    // 无符号 32 位解析：
    // base=0 允许十进制和 0x 前缀十六进制。
    bool parseU32(const std::string& s, uint32_t& out) {
        try {
            const auto v = std::stoull(s, nullptr, 0);
            if (v > 0xffffffffULL) return false;
            out = static_cast<uint32_t>(v);
            return true;
        } catch (...) {
            return false;
        }
    }

    // 无符号 64 位解析（支持 0x 前缀）。
    bool parseU64(const std::string& s, uint64_t& out) {
        try {
            out = std::stoull(s, nullptr, 0);
            return true;
        } catch (...) {
            return false;
        }
    }

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // 控制台编码与源码 UTF-8 字符串保持一致，避免中文日志乱码。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::setlocale(LC_ALL, ".UTF-8");

    // -----------------------------
    // 第一阶段：解析命令行参数
    // -----------------------------
    // 说明：
    // 1) 该阶段不触发任何外部依赖（不连 vsock，不初始化 bot）
    // 2) checkpoint 工具类参数可“只读/只改后立即退出”
    // 3) --print-cursor 属于独立调试模式，也会提前退出
    std::string checkpointPath = "bot_checkpoint.json";
    bool showCk = false;
    bool resetCk = false;
    bool setSomething = false;
    bool printCursor = false;
    std::string memBackend = "vsock";
    uint32_t vsockCid = 2;
    uint32_t vsockPort = 4050;
    uint32_t vsockTimeoutMs = 5000;
    uint32_t cursorIntervalMs = 100;
    uint32_t remotePort = 19090;
    uint64_t debugPid = 0;
    TradingCheckpoint forced{};
    bool forceNextOp = false, forceCycle = false, forceTarget = false;

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");

    for (int i = 1; i < argc; ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h") {
            printCheckpointHelp();
            return 0;
        }
        if (a == "--checkpoint" && i + 1 < argc) {
            checkpointPath = args[++i];
            continue;
        }
        if (a == "--show-checkpoint") { showCk = true; continue; }
        if (a == "--reset-checkpoint") { resetCk = true; continue; }
        if (a == "--set-next-op" && i + 1 < argc) {
            int v;
            if (!parseInt(args[++i], v)) { std::cerr << "bad --set-next-op\n"; return 2; }
            forced.next_op = v; forceNextOp = true; setSomething = true; continue;
        }
        if (a == "--set-cycle" && i + 1 < argc) {
            int v;
            if (!parseInt(args[++i], v)) { std::cerr << "bad --set-cycle\n"; return 2; }
            forced.cycle = v; forceCycle = true; setSomething = true; continue;
        }
        if (a == "--set-target-money" && i + 1 < argc) {
            int v;
            if (!parseInt(args[++i], v)) { std::cerr << "bad --set-target-money\n"; return 2; }
            forced.target_money = v; forceTarget = true; setSomething = true; continue;
        }
        if (a == "--print-cursor") {
            printCursor = true;
            continue;
        }
        if (a == "--cursor-interval-ms" && i + 1 < argc) {
            if (!parseU32(args[++i], cursorIntervalMs)) { std::cerr << "bad --cursor-interval-ms\n"; return 2; }
            continue;
        }
        if (a == "--pid" && i + 1 < argc) {
            if (!parseU64(args[++i], debugPid)) { std::cerr << "bad --pid\n"; return 2; }
            continue;
        }
        if (a == "--remote-control") {
            // 兼容旧参数：
            // 当前版本默认即启用 RuntimeController，因此该参数仅保留占位，
            // 避免旧脚本报错。
            continue;
        }
        if (a == "--remote-port" && i + 1 < argc) {
            if (!parseU32(args[++i], remotePort) || remotePort == 0 || remotePort > 65535) {
                std::cerr << "bad --remote-port\n";
                return 2;
            }
            continue;
        }
        if (a == "--mem-backend" && i + 1 < argc) {
            memBackend = args[++i];
            continue;
        }
        if (a == "--cid" && i + 1 < argc) {
            if (!parseU32(args[++i], vsockCid)) { std::cerr << "bad --cid\n"; return 2; }
            continue;
        }
        if (a == "--port" && i + 1 < argc) {
            if (!parseU32(args[++i], vsockPort)) { std::cerr << "bad --port\n"; return 2; }
            continue;
        }
        if (a == "--vsock-timeout-ms" && i + 1 < argc) {
            if (!parseU32(args[++i], vsockTimeoutMs)) { std::cerr << "bad --vsock-timeout-ms\n"; return 2; }
            continue;
        }
    }

    if (showCk || resetCk || setSomething) {
        // -----------------------------
        // 第二阶段：checkpoint 维护模式
        // -----------------------------
        // 命中该分支时，执行完对应操作后直接退出，不进入 bot 主流程。
        CheckpointStore store(checkpointPath);
        TradingCheckpoint ck;
        std::string err;
        bool has = store.load(ck, &err);
        if (!has && !err.empty()) std::cerr << "[Bot] checkpoint 读取失败: " << err << "\n";

        if (resetCk) {
            if (!store.clear(&err)) {
                std::cerr << "[Bot] checkpoint 删除失败: " << err << "\n";
                return 2;
            }
            std::cout << "[Bot] checkpoint 已删除: " << store.path().string() << "\n";
            return 0;
        }

        if (setSomething) {
            // 若当前不存在 checkpoint，则从默认 v3 结构创建。
            if (!has) ck = TradingCheckpoint{};
            ck.version = 3;
            if (forceNextOp) ck.next_op = forced.next_op;
            if (forceCycle) ck.cycle = forced.cycle;
            if (forceTarget) ck.target_money = forced.target_money;
            ck.last_op_name = "manually_set";

            if (!store.save(ck, &err)) {
                std::cerr << "[Bot] checkpoint 保存失败: " << err << "\n";
                return 2;
            }
            std::cout << "[Bot] checkpoint 已更新: " << store.path().string()
                      << " next_op=" << ck.next_op
                      << " cycle=" << ck.cycle
                      << " target=" << ck.target_money
                      << "\n";
            return 0;
        }

        if (showCk) {
            if (!has) {
                std::cout << "[Bot] 当前无 checkpoint: " << store.path().string() << "\n";
                return 0;
            }
            std::cout << "[Bot] checkpoint: " << store.path().string()
                      << " version=" << ck.version
                      << " next_op=" << ck.next_op
                      << " last=\"" << ck.last_op_name << "\""
                      << " cycle=" << ck.cycle
                      << " target=" << ck.target_money
                      << "\n";
            return 0;
        }
    }

    if (printCursor) {
        // -----------------------------
        // 第三阶段：光标/内存链调试模式
        // -----------------------------
        // 该模式用于在线验证：
        // - Win32 鼠标位置（GetCursorPos）
        // - 游戏内存链坐标（dllBase + pointer chain）
        // 两者会并行打印，便于快速确认偏移与读链是否正确。
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        if (cursorIntervalMs == 0) {
            cursorIntervalMs = 1;
        }
        if (debugPid == 0) {
            std::cerr << "[Bot] --print-cursor 需要同时指定 --pid\n";
            return 2;
        }

        if (memBackend != "vsock") {
            std::cerr << "unsupported --mem-backend in print-cursor: " << memBackend << " (expected vsock)\n";
            return 2;
        }
        std::shared_ptr<IProcessMemoryReader> debugReader =
            create_vsock_memory_reader(vsockCid, vsockPort, vsockTimeoutMs);
        std::string err;
        if (!debugReader->initialize_binding(debugPid, &err)) {
            std::cerr << "[Bot] print-cursor INIT_BIND 失败: " << err << "\n";
            return 2;
        }

        std::cout << "[Bot] 光标坐标打印模式已启动（Ctrl+C 停止），interval="
                  << cursorIntervalMs << "ms"
                  << " pid=" << debugPid
                  << " shared_addr=0x" << std::hex
                  << reinterpret_cast<std::uintptr_t>(&g_shared_data)
                  << std::dec << "\n";
        while (!g_stopRequested.load(std::memory_order_relaxed)) {
            POINT pt{};
            if (GetCursorPos(&pt)) {
                const SharedDataSnapshot snap = read_shared_data_snapshot();
                std::cout << "[Cursor] x=" << pt.x << " y=" << pt.y
                          << " sync=" << snap.sync_flag
                          << " ts=" << snap.timestamp
                          << " current_x=" << snap.current_x
                          << " current_y=" << snap.current_y
                          << " map_id=" << snap.map_id
                          << "\n";
            } else {
                std::cerr << "[Cursor] GetCursorPos failed\n";
            }
            Sleep(cursorIntervalMs);
        }
        return 0;
    }

    // -----------------------------
    // 第四阶段：正式运行模式
    // -----------------------------
    // 创建进程内存读取后端，并注入到 gameMemory。
    std::shared_ptr<IProcessMemoryReader> reader;
    if (memBackend == "vsock") {
        reader = create_vsock_memory_reader(vsockCid, vsockPort, vsockTimeoutMs);
    } else {
        std::cerr << "unsupported --mem-backend: " << memBackend << " (expected vsock)\n";
        return 2;
    }

    bot.gameMemory.set_memory_reader(reader);

    // Ctrl+C / 关闭窗口请求安全停止（保存 checkpoint）
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    try {
        // 运行控制接线说明：
        // - init 回调：只做初始化（不自动开跑）
        // - run  回调：真正执行跑商循环
        // - stop 回调：只置停止标志，不做阻塞操作
        // RuntimeController 负责接收 TCP 指令并驱动这三类回调。
        bot.configureRunControl(&g_stopRequested, "bot_checkpoint.json");
        runtime::ServerOptions options{};
        options.port = remotePort;
        runtime::RuntimeController controller(
            options,
            [&]() { bot.init(); },
            [&]() {
                g_stopRequested.store(false, std::memory_order_relaxed);
                bot.runTradingRoute();
            },
            [&]() { g_stopRequested.store(true, std::memory_order_relaxed); }
        );
        return controller.RunBlocking();
    } catch (const std::exception& e) {
        std::cerr << "[Bot] 异常退出: " << e.what() << "\n";
        return -2;
    }
}
