#include "TencentBot.h"

#include "BotLogger.h"
#include "domain/RunControl.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "../IbInputSimulator/Simulator/include/IbInputSimulator/InputSimulator.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

// 初始化阶段输入链路自检：
// 1) 读取当前鼠标位置
// 2) 发送一次相对移动
// 3) 再次读取位置并校验方向/幅度
// 4) 发送反向移动回原位
bool verifyIbMouseRelativeMove(std::string* reason) {
    POINT before{};
    if (!GetCursorPos(&before)) {
        if (reason) {
            *reason = "GetCursorPos(before) failed";
        }
        return false;
    }

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    if (screenW <= 0 || screenH <= 0) {
        if (reason) {
            *reason = "GetSystemMetrics returned invalid screen size";
        }
        return false;
    }

    int dx = (before.x <= screenW - 80) ? 40 : -40;
    int dy = (before.y <= screenH - 80) ? 28 : -28;
    if (dx == 0) {
        dx = 40;
    }
    if (dy == 0) {
        dy = 28;
    }

    if (!IbSendMouseMove(static_cast<uint32_t>(dx), static_cast<uint32_t>(dy), Send::MoveMode::Relative)) {
        if (reason) {
            *reason = "IbSendMouseMove(relative test move) failed";
        }
        return false;
    }

    domain::sleep_interruptible(nullptr, 40);

    POINT after{};
    if (!GetCursorPos(&after)) {
        if (reason) {
            *reason = "GetCursorPos(after) failed";
        }
        return false;
    }

    const int movedX = after.x - before.x;
    const int movedY = after.y - before.y;

    const bool signXOk = (dx == 0) || (movedX == 0 ? false : ((movedX > 0) == (dx > 0)));
    const bool signYOk = (dy == 0) || (movedY == 0 ? false : ((movedY > 0) == (dy > 0)));
    const bool magXOk = std::abs(movedX) >= std::max(6, std::abs(dx) / 2);
    const bool magYOk = std::abs(movedY) >= std::max(6, std::abs(dy) / 2);
    const bool ok = signXOk && signYOk && magXOk && magYOk;

    if (movedX != 0 || movedY != 0) {
        IbSendMouseMove(static_cast<uint32_t>(-movedX), static_cast<uint32_t>(-movedY), Send::MoveMode::Relative);
        domain::sleep_interruptible(nullptr, 20);
    }

    if (!ok && reason) {
        std::ostringstream oss;
        oss << "relative move mismatch"
            << " expected=(" << dx << "," << dy << ")"
            << " actual=(" << movedX << "," << movedY << ")";
        *reason = oss.str();
    }
    return ok;
}

} // namespace

TencentBot::TencentBot()
    : vision(screenCapture) {}

TencentBot::~TencentBot() {
    IbSendDestroy();
    screenCapture.release();
}

void TencentBot::configureRunControl(std::atomic_bool* stopSignal, const std::string& checkpointFile) {
    stopSignal_ = stopSignal;
    if (!checkpointFile.empty()) {
        checkpointFile_ = checkpointFile;
    }
}

void TencentBot::init() {
    BOT_LOG("TencentBot", "========== 开始初始化 ==========");

    uint64_t uiProcessPid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        BOT_ERR("TencentBot", "CreateToolhelp32Snapshot 失败");
        throw std::runtime_error("CreateToolhelp32Snapshot failed");
    }

    PROCESSENTRY32 processEntry{};
    processEntry.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(snapshot, &processEntry)) {
        do {
            std::string exeName(processEntry.szExeFile);
            if (exeName.find("mhmain.exe") != std::string::npos) {
                gameMemory.processIds.push_back(processEntry.th32ProcessID);
                BOT_LOG("TencentBot", "发现游戏主进程 PID=" << processEntry.th32ProcessID);
            }
            if (exeName.find("mhtab.exe") != std::string::npos) {
                uiProcessPid = processEntry.th32ProcessID;
                BOT_LOG("TencentBot", "发现 UI 进程 PID=" << uiProcessPid);
            }
        } while (Process32Next(snapshot, &processEntry));
    }
    CloseHandle(snapshot);

    if (gameMemory.processIds.empty()) {
        BOT_ERR("TencentBot", "未发现 mhmain.exe 进程");
        throw std::runtime_error("mhmain.exe not found");
    }
    if (uiProcessPid == 0) {
        BOT_ERR("TencentBot", "未发现 mhtab.exe 进程");
        throw std::runtime_error("mhtab.exe not found");
    }

    std::string initErr;
    if (!gameMemory.initialize_module_bases("mhmain.dll", &initErr)) {
        BOT_ERR("TencentBot", "初始化模块基址失败: " << initErr);
        throw std::runtime_error("initialize_module_bases failed");
    }

    const auto ibInitErr = IbSendInit(Send::SendType::Logitech, 0, nullptr);
    if (ibInitErr != Send::Error::Success) {
        BOT_ERR("TencentBot", "输入模拟器初始化失败, error=" << static_cast<int>(ibInitErr));
        throw std::runtime_error("IbSendInit failed");
    }
    BOT_LOG("TencentBot", "鼠标模拟器初始化完成 (Logitech)");

    std::string ibVerifyErr;
    if (!verifyIbMouseRelativeMove(&ibVerifyErr)) {
        BOT_ERR("TencentBot", "输入模拟器自检失败: " << ibVerifyErr);
        throw std::runtime_error("IbInputSimulator movement verification failed");
    }
    BOT_LOG("TencentBot", "输入模拟器自检通过 (relative move)");

    aiCaptcha = std::make_unique<CaptchaEngine>("http://127.0.0.1:8000");
    BOT_LOG("TencentBot", "CaptchaEngine 已连接 http://127.0.0.1:8000");

    if (!screenCapture.initByPid(static_cast<DWORD>(uiProcessPid))) {
        BOT_ERR("TencentBot", "DXGI 窗口捕获初始化失败 PID=" << uiProcessPid);
        throw std::runtime_error("DxgiWindowCapture init failed");
    }
    BOT_LOG("TencentBot", "DXGI 窗口捕获初始化成功");

    if (!vision.loadAllTemplates()) {
        BOT_ERR("TencentBot", "视觉模板加载失败");
        throw std::runtime_error("VisionEngine template load failed");
    }

    BOT_LOG("TencentBot", "========== 初始化完成 ==========");
}
