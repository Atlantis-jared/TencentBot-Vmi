#include <iostream>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <windows.h>
#include "hv.h"
#include "src/CheckpointStore.h"
#include "src/TencentBot.h"
#include "src/DxgiWindowCapture.h"
#include "src/CaptchaEngine.h"
#include "src/MemflowConnector.h"

TencentBot bot;
MemflowConnector memflow;
static std::atomic_bool g_stopRequested{false};

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stopRequested.store(true, std::memory_order_relaxed);
            return TRUE; // handled (let app stop safely and checkpoint)
        default:
            return FALSE;
    }
}

namespace {
    constexpr unsigned kScreenLogicalW = 2560;
    constexpr unsigned kScreenLogicalH = 1440;
    constexpr unsigned kMouseMax = 65535;
}

namespace {
    void printCheckpointHelp() {
        std::cout <<
R"(Checkpoint tools:
  --checkpoint <path>         checkpoint 文件路径（默认 bot_checkpoint.json）
  --show-checkpoint           打印当前 checkpoint 并退出
  --reset-checkpoint          删除 checkpoint（下次从头开始）并退出
  --set-next-op <n>           设置 next_op 并退出
  --set-cycle <n>             设置 cycle 并退出
  --set-target-money <n>      设置目标银票（默认 150000）并退出

next_op 对应关系（v3）:
  0: leave_gang_to_difu        出帮派 -> 长安 -> 大唐国境 -> 地府
  1: difu_buy_paper            地府买纸钱
  2: travel_to_beiju           地府 -> ... -> 北俱
  3: beiju_sell_paper_buy_oil  北俱卖纸钱 + 买油（卖完会立即判断是否回帮派）
  4: return_to_difu            北俱 -> ... -> 地府
  5: difu_sell_oil             地府卖油（卖完会立即判断是否回帮派）
)";
    }

    bool parseInt(const std::string& s, int& out) {
        try { out = std::stoi(s); return true; } catch (...) { return false; }
    }
} // namespace

int main(int argc, char** argv) {
    // Parse args for checkpoint management (no HV required)
    std::string checkpointPath = "bot_checkpoint.json";
    bool showCk = false;
    bool resetCk = false;
    bool setSomething = false;
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
    }

    if (showCk || resetCk || setSomething) {
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
            // if no existing ck, start from defaults v3
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

    // 检查 HV 是否运行
    if (!hv::is_hv_running()) {
        std::wcout << L"HV not running.\n";
        return -1;
    }

    // Ctrl+C / 关闭窗口请求安全停止（保存 checkpoint）
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    auto const hv_base = static_cast<uint8_t*>(hv::get_hv_base());
    auto const hv_size = 0x64000;
    // 隐藏 hypervisor
    hv::for_each_cpu([&](uint32_t) {
        for (size_t i = 0; i < hv_size; i += 0x1000) {
            auto const virt = hv_base + i;
            auto const phys = hv::get_physical_address(0, virt);



            
            hv::hide_physical_page(phys >> 12);
        }
    });

    // 确保退出时清理 HV（无论是否中断）
    struct HvCleanupGuard {
        ~HvCleanupGuard() {
            hv::for_each_cpu([](uint32_t) {
                hv::remove_all_mmrs();
            });
        }
    } hvGuard;

    try {
        bot.init();

        // 启动 Memflow 迁移 (如果游戏进程已找到)
        if (!bot.gameMemory.processIds.empty()) {
            bot.gameMemory.setMemflow(&memflow);
            memflow.start((uint32_t)bot.gameMemory.processIds[0], bot.gameMemory.dllBaseAddrs[0]);
        }

        bot.configureRunControl(&g_stopRequested, "bot_checkpoint.json");
        std::cout << "[Bot] 初始化完成（按 Enter 或 Ctrl+C 请求停止，可续跑）\n";

        // 监听控制台 Enter（不阻塞主线程）
        std::thread([] {
            std::string line;
            std::getline(std::cin, line);
            g_stopRequested.store(true, std::memory_order_relaxed);
        }).detach();

        IbSendMouseMove(static_cast<int>(kMouseMax * 1280 / kScreenLogicalW),
                        static_cast<int>(kMouseMax * 40 / kScreenLogicalH),
                        Send::MoveMode::Absolute);
        Sleep(100);
        IbSendMouseClick(Send::MouseButton::Left);
        Sleep(100);
        IbSendMouseMove(static_cast<int>(kMouseMax * 1280 / kScreenLogicalW),
                        static_cast<int>(kMouseMax * 200 / kScreenLogicalH),
                        Send::MoveMode::Absolute);
        Sleep(100);

        bot.runTradingRoute();
    } catch (const std::exception& e) {
        std::cerr << "[Bot] 异常退出: " << e.what() << "\n";
        return -2;
    }

    return 0;
}