#pragma once
// =============================================================================
// BotLogger.h — 统一日志工具（Header-only）
// =============================================================================
// 提供三个日志宏，在整个 Bot 项目中统一使用，避免各处直接写 std::cout/cerr。
//
// 用法示例：
//   BOT_LOG("GameMemory", "进程 PID=" << pid << " CR3=0x" << std::hex << cr3);
//   BOT_WARN("TencentBot", "传送重试 " << retryCount << " 次");
//   BOT_ERR("VisionEngine", "模板加载失败: " << filePath);
//
// 输出格式：
//   [2026-02-19 16:09:00] [INFO ] [GameMemory  ] 进程 PID=1234 CR3=0xabcd
//   [2026-02-19 16:09:01] [WARN ] [TencentBot  ] 传送重试 1 次
//   [2026-02-19 16:09:02] [ERROR] [VisionEngine] 模板加载失败: maps/foo.png
// =============================================================================

#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace BotLogger {

// -----------------------------------------------------------------------------
// currentTimestamp() — 返回当前本地时间字符串，格式 "YYYY-MM-DD HH:MM:SS"
// -----------------------------------------------------------------------------
inline std::string currentTimestamp() {
    auto now        = std::chrono::system_clock::now();
    auto epochSecs  = std::chrono::system_clock::to_time_t(now);
    std::tm localTm{};
#ifdef _WIN32
    localtime_s(&localTm, &epochSecs); // Windows 安全版本
#else
    localtime_r(&epochSecs, &localTm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&localTm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// -----------------------------------------------------------------------------
// formatModuleName() — 将模块名左对齐填充到固定宽度，使多行日志列对齐
// 例：formatModuleName("GameMemory", 12) => "GameMemory  "
// -----------------------------------------------------------------------------
inline std::string formatModuleName(const char* moduleName, int paddedWidth = 12) {
    std::string name(moduleName);
    if (static_cast<int>(name.size()) < paddedWidth)
        name.append(static_cast<size_t>(paddedWidth - static_cast<int>(name.size())), ' ');
    return name;
}

// 全局输出互斥：所有模块统一复用，避免多线程日志互相插行。
inline std::mutex& outputMutex() {
    static std::mutex m;
    return m;
}

inline void writeLine(std::ostream& os, const char* level, const char* moduleName, const std::string& message) {
    std::lock_guard<std::mutex> lk(outputMutex());
    os << "[" << currentTimestamp() << "] [" << level << "] ["
       << formatModuleName(moduleName) << "] " << message << "\n";
    os.flush();
}

} // namespace BotLogger

// =============================================================================
// 日志宏 — 使用流式语法，支持任意类型拼接
// =============================================================================

/// 普通信息日志，输出到 stdout
#define BOT_LOG(module, msg) \
    do { \
        std::ostringstream _bot_log_oss; \
        _bot_log_oss << msg; \
        BotLogger::writeLine(std::cout, "INFO ", module, _bot_log_oss.str()); \
    } while (0)

/// 警告日志，输出到 stdout（黄色语义，但终端颜色依赖环境，不强制转义码）
#define BOT_WARN(module, msg) \
    do { \
        std::ostringstream _bot_warn_oss; \
        _bot_warn_oss << msg; \
        BotLogger::writeLine(std::cout, "WARN ", module, _bot_warn_oss.str()); \
    } while (0)

/// 错误日志，输出到 stderr
#define BOT_ERR(module, msg) \
    do { \
        std::ostringstream _bot_err_oss; \
        _bot_err_oss << msg; \
        BotLogger::writeLine(std::cerr, "ERROR", module, _bot_err_oss.str()); \
    } while (0)
