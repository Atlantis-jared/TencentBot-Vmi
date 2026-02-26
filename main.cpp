#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <iostream>
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <clocale>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "src/CheckpointStore.h"
#include "src/TencentBot.h"
#include "src/DxgiWindowCapture.h"
#include "src/CaptchaEngine.h"
#include "src/MemoryReader.h"

#pragma comment(lib, "Ws2_32.lib")

TencentBot bot;
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

    bool parseInt(const std::string& s, int& out) {
        try { out = std::stoi(s); return true; } catch (...) { return false; }
    }

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

    bool parseU64(const std::string& s, uint64_t& out) {
        try {
            out = std::stoull(s, nullptr, 0);
            return true;
        } catch (...) {
            return false;
        }
    }

    std::string trim(const std::string& s) {
        std::size_t begin = 0;
        while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
            ++begin;
        }
        std::size_t end = s.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
            --end;
        }
        return s.substr(begin, end - begin);
    }

    std::string upper_ascii(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return s;
    }

    class WsaInitGuard {
    public:
        WsaInitGuard() {
            WSADATA wsa{};
            ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        }

        ~WsaInitGuard() {
            if (ok_) {
                WSACleanup();
            }
        }

        bool ok() const { return ok_; }

    private:
        bool ok_ = false;
    };
} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Keep console code page aligned with /utf-8 string literals.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::setlocale(LC_ALL, ".UTF-8");

    // Parse args for checkpoint management (no HV required)
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

    if (printCursor) {
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        if (cursorIntervalMs == 0) {
            cursorIntervalMs = 1;
        }
        if (debugPid == 0) {
            std::cerr << "[Bot] --print-cursor 需要同时指定 --pid\n";
            return 2;
        }

        std::shared_ptr<IProcessMemoryReader> debugReader;
        if (memBackend == "vsock") {
            debugReader = create_vsock_memory_reader(vsockCid, vsockPort, vsockTimeoutMs);
        } else {
            std::cerr << "unsupported --mem-backend in print-cursor: " << memBackend << " (expected vsock)\n";
            return 2;
        }

        std::uint64_t dllBase = 0;
        std::string err;
        if (!debugReader->query_module_base_by_pid(debugPid, "mhmain.dll", &dllBase, &err)) {
            std::cerr << "[Bot] print-cursor 查询 mhmain.dll 基址失败: " << err << "\n";
            return 2;
        }

        std::cout << "[Bot] 光标坐标打印模式已启动（Ctrl+C 停止），interval="
                  << cursorIntervalMs << "ms"
                  << " pid=" << debugPid
                  << " dllBase=0x" << std::hex << dllBase << std::dec << "\n";
        while (!g_stopRequested.load(std::memory_order_relaxed)) {
            POINT pt{};
            if (GetCursorPos(&pt)) {
                const std::uint64_t vaFirstPtr = dllBase + GAME_PIT_CHAIN_BASE_OFFSET;
                std::uint64_t firstPtr = 0;
                std::uint32_t rawX = 0;
                std::uint32_t rawY = 0;
                std::string readErr;
                if (!debugReader->read_virtual_by_pid(debugPid, vaFirstPtr, &firstPtr, sizeof(firstPtr), &readErr)) {
                    std::cerr << "[Cursor] x=" << pt.x << " y=" << pt.y
                              << " pid=" << debugPid
                              << " dllBase=0x" << std::hex << dllBase
                              << " va(first_ptr)=0x" << vaFirstPtr
                              << std::dec
                              << " err=" << readErr << "\n";
                } else {
                    const std::uint64_t vaRawX = firstPtr + GAME_PIT_POS_STRUCT_OFFSET;
                    const std::uint64_t vaRawY = vaRawX + sizeof(std::uint32_t);
                    const bool okX = debugReader->read_virtual_by_pid(debugPid, vaRawX, &rawX, sizeof(rawX), &readErr);
                    const bool okY = debugReader->read_virtual_by_pid(debugPid, vaRawY, &rawY, sizeof(rawY), &readErr);
                    std::cout << "[Cursor] x=" << pt.x << " y=" << pt.y
                              << " pid=" << debugPid
                              << " dllBase=0x" << std::hex << dllBase
                              << " va(first_ptr)=0x" << vaFirstPtr
                              << " firstPtr=0x" << firstPtr
                              << " va(rawX)=0x" << vaRawX
                              << " va(rawY)=0x" << vaRawY
                              << std::dec;
                    if (okX && okY) {
                        std::cout << " rawX=" << rawX << " rawY=" << rawY << "\n";
                    } else {
                        std::cout << " err=" << readErr << "\n";
                    }
                }
            } else {
                std::cerr << "[Cursor] GetCursorPos failed\n";
            }
            Sleep(cursorIntervalMs);
        }
        return 0;
    }

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
        bot.configureRunControl(&g_stopRequested, "bot_checkpoint.json");

        WsaInitGuard wsa;
        if (!wsa.ok()) {
            std::cerr << "[Bot] 远程控制启动失败: WSAStartup 失败\n";
            return 2;
        }

        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            std::cerr << "[Bot] 远程控制启动失败: socket 创建失败\n";
            return 2;
        }

        auto closeSock = [](SOCKET& s) {
            if (s != INVALID_SOCKET) {
                closesocket(s);
                s = INVALID_SOCKET;
            }
        };

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(remotePort));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "[Bot] 远程控制启动失败: bind 端口 " << remotePort << " 失败\n";
            closeSock(listenSock);
            return 2;
        }
        if (listen(listenSock, 8) == SOCKET_ERROR) {
            std::cerr << "[Bot] 远程控制启动失败: listen 失败\n";
            closeSock(listenSock);
            return 2;
        }

        std::atomic_bool remoteQuit{false};
        std::atomic_bool initRequested{false};
        std::atomic_bool startRequested{false};
        constexpr int kStateNotInitialized = 0;
        constexpr int kStateInitializing = 1;
        constexpr int kStateIdle = 2;
        constexpr int kStateRunning = 3;
        constexpr int kStateInitFailed = 4;
        std::atomic<int> runState{kStateNotInitialized};

        std::thread worker([&] {
            while (!remoteQuit.load(std::memory_order_relaxed)) {
                if (initRequested.exchange(false, std::memory_order_acq_rel)) {
                    const int state = runState.load(std::memory_order_relaxed);
                    if (state == kStateNotInitialized || state == kStateInitFailed) {
                        runState.store(kStateInitializing, std::memory_order_relaxed);
                        try {
                            bot.init();
                            runState.store(kStateIdle, std::memory_order_relaxed);
                        } catch (const std::exception& e) {
                            std::cerr << "[Bot] init 异常: " << e.what() << "\n";
                            runState.store(kStateInitFailed, std::memory_order_relaxed);
                        }
                    }
                }

                if (startRequested.exchange(false, std::memory_order_acq_rel)) {
                    const int state = runState.load(std::memory_order_relaxed);
                    if (state != kStateIdle) {
                        Sleep(20);
                        continue;
                    }

                    g_stopRequested.store(false, std::memory_order_relaxed);
                    runState.store(kStateRunning, std::memory_order_relaxed);
                    try {
                        bot.runTradingRoute();
                    } catch (const std::exception& e) {
                        std::cerr << "[Bot] runTradingRoute 异常: " << e.what() << "\n";
                    }
                    if (!remoteQuit.load(std::memory_order_relaxed)) {
                        runState.store(kStateIdle, std::memory_order_relaxed);
                    }
                }
                Sleep(20);
            }
        });

        std::cout << "[Bot] 远程控制已开启，监听 0.0.0.0:" << remotePort << "\n";
        std::cout << "[Bot] 当前状态: NOT_INITIALIZED（等待 INIT）\n";
        std::cout << "[Bot] 指令: INIT / START / STOP / STATUS / QUIT\n";
        while (!remoteQuit.load(std::memory_order_relaxed)) {
            SOCKET clientSock = accept(listenSock, nullptr, nullptr);
            if (clientSock == INVALID_SOCKET) {
                break;
            }

            char buf[256] = {};
            const int rc = recv(clientSock, buf, static_cast<int>(sizeof(buf) - 1), 0);
            std::string reply = "ERR empty command\n";
            if (rc > 0) {
                std::string cmd(buf, buf + rc);
                cmd = upper_ascii(trim(cmd));
                if (cmd == "INIT") {
                    const int state = runState.load(std::memory_order_relaxed);
                    if (state == kStateNotInitialized || state == kStateInitFailed) {
                        initRequested.store(true, std::memory_order_relaxed);
                        reply = "OK init requested\n";
                    } else if (state == kStateInitializing) {
                        reply = "OK initializing\n";
                    } else {
                        reply = "OK already initialized\n";
                    }
                } else if (cmd == "START") {
                    const int state = runState.load(std::memory_order_relaxed);
                    if (state == kStateIdle) {
                        startRequested.store(true, std::memory_order_relaxed);
                        reply = "OK starting\n";
                    } else if (state == kStateInitializing) {
                        reply = "ERR still initializing\n";
                    } else if (state == kStateNotInitialized) {
                        reply = "ERR not initialized, send INIT first\n";
                    } else if (state == kStateInitFailed) {
                        reply = "ERR init failed, send INIT again\n";
                    } else {
                        reply = "OK already running\n";
                    }
                } else if (cmd == "STOP") {
                    g_stopRequested.store(true, std::memory_order_relaxed);
                    reply = "OK stop requested\n";
                } else if (cmd == "STATUS") {
                    const int state = runState.load(std::memory_order_relaxed);
                    if (state == kStateNotInitialized) {
                        reply = "NOT_INITIALIZED\n";
                    } else if (state == kStateInitializing) {
                        reply = "INITIALIZING\n";
                    } else if (state == kStateIdle) {
                        reply = "IDLE\n";
                    } else if (state == kStateRunning) {
                        reply = "RUNNING\n";
                    } else if (state == kStateInitFailed) {
                        reply = "INIT_FAILED\n";
                    } else {
                        reply = "UNKNOWN\n";
                    }
                } else if (cmd == "QUIT" || cmd == "EXIT") {
                    g_stopRequested.store(true, std::memory_order_relaxed);
                    remoteQuit.store(true, std::memory_order_relaxed);
                    reply = "OK quitting\n";
                } else {
                    reply = "ERR unknown command\n";
                }
            }
            send(clientSock, reply.data(), static_cast<int>(reply.size()), 0);
            closeSock(clientSock);
        }

        remoteQuit.store(true, std::memory_order_relaxed);
        g_stopRequested.store(true, std::memory_order_relaxed);
        if (worker.joinable()) {
            worker.join();
        }
        closeSock(listenSock);
    } catch (const std::exception& e) {
        std::cerr << "[Bot] 异常退出: " << e.what() << "\n";
        return -2;
    }

    return 0;
}
