#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "RuntimeController.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace runtime {

namespace {

// ---------------------------------------------------------------------------
// 运行时生命周期状态机（工业化控制核心）
// ---------------------------------------------------------------------------
enum class RuntimeState {
    NotInitialized = 0,
    Initializing = 1,
    Idle = 2,
    Running = 3,
    InitFailed = 4
};

// 统一状态字符串，便于 STATUS 返回和结构化日志一致。
const char* state_to_text(RuntimeState state) {
    switch (state) {
        case RuntimeState::NotInitialized: return "NOT_INITIALIZED";
        case RuntimeState::Initializing: return "INITIALIZING";
        case RuntimeState::Idle: return "IDLE";
        case RuntimeState::Running: return "RUNNING";
        case RuntimeState::InitFailed: return "INIT_FAILED";
        default: return "UNKNOWN";
    }
}

// 返回本地时间（毫秒精度），用于日志时间戳。
std::string now_text() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::ostringstream oss;
    oss << st.wYear << "-";
    if (st.wMonth < 10) oss << '0';
    oss << st.wMonth << "-";
    if (st.wDay < 10) oss << '0';
    oss << st.wDay << " ";
    if (st.wHour < 10) oss << '0';
    oss << st.wHour << ":";
    if (st.wMinute < 10) oss << '0';
    oss << st.wMinute << ":";
    if (st.wSecond < 10) oss << '0';
    oss << st.wSecond << ".";
    if (st.wMilliseconds < 100) oss << '0';
    if (st.wMilliseconds < 10) oss << '0';
    oss << st.wMilliseconds;
    return oss.str();
}

// JSON 最小转义，避免日志字段破坏 JSON 格式。
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

// 统一结构化日志输出：
// 设计目标：便于后续接入 ELK/ClickHouse/向量检索等系统。
void log_event(
    const char* event,
    const std::string& request_id,
    const std::string& command,
    RuntimeState state,
    const std::string& message
) {
    std::cout << "{\"ts\":\"" << now_text()
              << "\",\"module\":\"RuntimeController\""
              << ",\"event\":\"" << json_escape(event ? event : "")
              << "\",\"request_id\":\"" << json_escape(request_id)
              << "\",\"command\":\"" << json_escape(command)
              << "\",\"state\":\"" << state_to_text(state)
              << "\",\"message\":\"" << json_escape(message)
              << "\"}\n";
}

// 去除命令首尾空白。
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

// ASCII 大写化（命令关键字统一大小写处理）。
std::string upper_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

// WSA 生命周期守卫，确保初始化/清理配对。
class WsaInitGuard {
public:
    WsaInitGuard() {
        WSADATA wsa{};
        ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    ~WsaInitGuard() {
        if (ok_) WSACleanup();
    }
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

// 命令解析结果：
// 支持两种格式：
// 1) COMMAND
// 2) REQUEST_ID COMMAND
struct ParsedCommand {
    std::string request_id;
    std::string command;
};

// 白名单命令集合，控制端口只允许固定指令。
bool is_known_command(const std::string& cmd) {
    return cmd == "INIT" || cmd == "START" || cmd == "STOP" ||
           cmd == "STATUS" || cmd == "QUIT" || cmd == "EXIT";
}

// 解析命令行文本：
// - 如果第一个 token 是命令 -> 无 request_id
// - 否则尝试第二个 token 为命令 -> 第一个 token 视为 request_id
bool parse_command_line(const std::string& raw, ParsedCommand* out) {
    if (out == nullptr) return false;
    out->request_id.clear();
    out->command.clear();

    const std::string text = trim(raw);
    if (text.empty()) return false;

    std::istringstream iss(text);
    std::string t1;
    std::string t2;
    if (!(iss >> t1)) return false;
    t1 = upper_ascii(t1);
    if (is_known_command(t1)) {
        out->command = t1;
        return true;
    }

    if (!(iss >> t2)) return false;
    t2 = upper_ascii(t2);
    if (!is_known_command(t2)) return false;

    out->request_id = trim(raw.substr(0, raw.find_first_of(" \t\r\n")));
    out->command = t2;
    return true;
}

// 响应附带 request_id（若请求提供），用于端到端追踪。
std::string with_request_id(const std::string& body, const std::string& request_id) {
    if (request_id.empty()) {
        return body;
    }
    std::string line = body;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    line += " req=" + request_id + "\n";
    return line;
}

} // namespace

RuntimeController::RuntimeController(ServerOptions options, InitFn init_fn, RunFn run_fn, StopFn stop_fn)
    : options_(options),
      init_fn_(std::move(init_fn)),
      run_fn_(std::move(run_fn)),
      stop_fn_(std::move(stop_fn)) {}

int RuntimeController::RunBlocking() {
    // 回调检查：这 3 个回调是控制器与业务层解耦的关键接口。
    if (!init_fn_ || !run_fn_ || !stop_fn_) {
        std::cerr << "[RuntimeController] invalid callbacks\n";
        return 2;
    }

    // 网络子系统初始化。
    WsaInitGuard wsa;
    if (!wsa.ok()) {
        std::cerr << "[RuntimeController] WSAStartup failed\n";
        return 2;
    }

    // 创建监听 socket。
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        std::cerr << "[RuntimeController] socket create failed\n";
        return 2;
    }

    // 小工具：关闭 socket 并置 INVALID_SOCKET。
    auto close_sock = [](SOCKET& sock) {
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
    };

    // 绑定并监听控制端口。
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(options_.port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[RuntimeController] bind port " << options_.port << " failed\n";
        close_sock(listen_sock);
        return 2;
    }
    if (listen(listen_sock, 8) == SOCKET_ERROR) {
        std::cerr << "[RuntimeController] listen failed\n";
        close_sock(listen_sock);
        return 2;
    }

    // -------------------------
    // 控制状态与并发模型
    // -------------------------
    // server_quit: 接收线程退出标记
    // state/mu/cv : 运行时状态机与命令队列同步
    // *_requested : 控制命令事件（INIT/START/SHUTDOWN）
    bool server_quit = false;
    std::mutex mu;
    std::condition_variable cv;
    RuntimeState state = RuntimeState::NotInitialized;
    bool init_requested = false;
    bool start_requested = false;
    bool shutdown_requested = false;
    std::string last_error;

    // Worker 线程：
    // 专职执行 INIT 和 START，避免网络线程阻塞在长耗时任务中。
    std::thread worker([&] {
        while (true) {
            bool do_init = false;
            bool do_start = false;
            // 等待新的控制事件（或退出）。
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] {
                    return shutdown_requested || init_requested || start_requested;
                });
                if (shutdown_requested) {
                    break;
                }
                do_init = init_requested;
                do_start = start_requested;
                init_requested = false;
                start_requested = false;
            }

            // 处理 INIT：仅允许在未初始化/初始化失败时再次初始化。
            if (do_init) {
                bool should_init = false;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    if (state == RuntimeState::NotInitialized || state == RuntimeState::InitFailed) {
                        state = RuntimeState::Initializing;
                        last_error.clear();
                        should_init = true;
                    }
                }
                // 真实初始化放在锁外执行，避免长时间占用锁。
                if (should_init) {
                    try {
                        init_fn_();
                        std::lock_guard<std::mutex> lk(mu);
                        state = RuntimeState::Idle;
                        log_event("init_ok", "", "INIT", state, "initialized");
                    } catch (const std::exception& e) {
                        std::lock_guard<std::mutex> lk(mu);
                        state = RuntimeState::InitFailed;
                        last_error = e.what();
                        log_event("init_fail", "", "INIT", state, last_error);
                    }
                }
            }

            // 处理 START：仅允许在 IDLE 状态启动跑商。
            if (do_start) {
                bool can_start = false;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    if (state == RuntimeState::Idle) {
                        state = RuntimeState::Running;
                        last_error.clear();
                        can_start = true;
                    }
                }
                if (!can_start) {
                    continue;
                }

                // 真实跑商执行（阻塞），期间网络线程仍可收 STOP/STATUS。
                try {
                    run_fn_();
                    std::lock_guard<std::mutex> lk(mu);
                    if (!shutdown_requested) {
                        state = RuntimeState::Idle;
                    }
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lk(mu);
                    last_error = e.what();
                    if (!shutdown_requested) {
                        state = RuntimeState::Idle;
                    }
                    log_event("run_fail", "", "START", state, last_error);
                }
            }
        }
    });

    // 启动提示。
    std::cout << "[Bot] 远程控制已开启，监听 0.0.0.0:" << options_.port << "\n";
    std::cout << "[Bot] 当前状态: NOT_INITIALIZED（等待 INIT）\n";
    std::cout << "[Bot] 指令: INIT / START / STOP / STATUS / QUIT\n";

    // 网络线程：接收命令并快速返回，不做重业务执行。
    while (!server_quit) {
        SOCKET client_sock = accept(listen_sock, nullptr, nullptr);
        if (client_sock == INVALID_SOCKET) {
            break;
        }

        // 每连接读取一条命令。
        std::array<char, 512> buf{};
        const int rc = recv(client_sock, buf.data(), static_cast<int>(buf.size() - 1), 0);
        std::string reply = "ERR empty command\n";
        ParsedCommand parsed{};
        if (rc > 0) {
            std::string cmd_line(buf.data(), buf.data() + rc);
            // 命令解析失败直接返回协议错误。
            if (!parse_command_line(cmd_line, &parsed)) {
                reply = "ERR invalid command format\n";
            } else {
                // 快照当前状态，用于决策和日志。
                RuntimeState cur_state = RuntimeState::NotInitialized;
                {
                    std::lock_guard<std::mutex> lk(mu);
                    cur_state = state;
                }

                // INIT: 只触发初始化请求，不在网络线程直接执行。
                if (parsed.command == "INIT") {
                    if (cur_state == RuntimeState::NotInitialized || cur_state == RuntimeState::InitFailed) {
                        {
                            std::lock_guard<std::mutex> lk(mu);
                            init_requested = true;
                        }
                        cv.notify_one();
                        reply = "OK init requested\n";
                    } else if (cur_state == RuntimeState::Initializing) {
                        reply = "OK initializing\n";
                    } else {
                        reply = "OK already initialized\n";
                    }
                // START: 只在 IDLE 下触发，禁止未初始化直接运行。
                } else if (parsed.command == "START") {
                    if (cur_state == RuntimeState::Idle) {
                        {
                            std::lock_guard<std::mutex> lk(mu);
                            start_requested = true;
                        }
                        cv.notify_one();
                        reply = "OK starting\n";
                    } else if (cur_state == RuntimeState::Initializing) {
                        reply = "ERR still initializing\n";
                    } else if (cur_state == RuntimeState::NotInitialized) {
                        reply = "ERR not initialized, send INIT first\n";
                    } else if (cur_state == RuntimeState::InitFailed) {
                        reply = "ERR init failed, send INIT again\n";
                    } else {
                        reply = "OK already running\n";
                    }
                // STOP: 仅发停止信号，请求业务层安全退出。
                } else if (parsed.command == "STOP") {
                    try {
                        stop_fn_();
                    } catch (const std::exception& e) {
                        log_event("stop_fail", parsed.request_id, parsed.command, cur_state, e.what());
                    }
                    if (cur_state == RuntimeState::Running) {
                        reply = "OK stop requested\n";
                    } else {
                        reply = "OK not running\n";
                    }
                // STATUS: 返回状态机文本，InitFailed 会附带错误原因。
                } else if (parsed.command == "STATUS") {
                    std::string err;
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        cur_state = state;
                        err = last_error;
                    }
                    reply = std::string(state_to_text(cur_state));
                    if (cur_state == RuntimeState::InitFailed && !err.empty()) {
                        reply += " " + err;
                    }
                    reply += "\n";
                // QUIT: 请求 stop + 关闭服务。
                } else if (parsed.command == "QUIT" || parsed.command == "EXIT") {
                    try {
                        stop_fn_();
                    } catch (const std::exception& e) {
                        log_event("stop_fail", parsed.request_id, parsed.command, cur_state, e.what());
                    }
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        shutdown_requested = true;
                    }
                    cv.notify_one();
                    server_quit = true;
                    reply = "OK quitting\n";
                } else {
                    reply = "ERR unknown command\n";
                }

                // 命令级结构化日志。
                log_event("command", parsed.request_id, parsed.command, cur_state, trim(reply));
            }
        }

        // 回写请求 ID（如果有）。
        reply = with_request_id(reply, parsed.request_id);
        send(client_sock, reply.data(), static_cast<int>(reply.size()), 0);
        close_sock(client_sock);
    }

    // 统一收尾：发 stop、唤醒 worker、等待线程退出、关闭 socket。
    try {
        stop_fn_();
    } catch (const std::exception& e) {
        log_event("stop_fail", "", "STOP", RuntimeState::Running, e.what());
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        shutdown_requested = true;
    }
    cv.notify_one();
    if (worker.joinable()) {
        worker.join();
    }
    close_sock(listen_sock);
    return 0;
}

} // namespace runtime
