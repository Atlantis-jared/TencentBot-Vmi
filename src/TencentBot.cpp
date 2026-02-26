// =============================================================================
// TencentBot.cpp — 跑商机器人主类实现（Part 1: 初始化 + 底层操作 + 导航路线）
// =============================================================================
#include "TencentBot.h"
#include "domain/TradingRouteBehavior.h"
#include "BotLogger.h"
#include "CheckpointStore.h"

// --- Windows 宏冲突修正 ---
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <windows.h>
#include <atomic>
#include <cmath>
#include <random>
#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <tlhelp32.h>
#include <utility>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

// =============================================================================
// 全局常量
// =============================================================================

// --- 地图 UI 物理尺寸表（用于小地图坐标 → 屏幕像素换算）---
const std::map<std::string, MapProperties> MAP_PROPERTIES = {
    {"jianyecheng",     {556, 277, 288, 144}},
    {"donghaiwan",      {276, 276, 119, 119}},
    {"changancheng",    {545, 276, 548, 278}},
    {"difu",            {369, 276, 159, 119}},
    {"huaguoshan",      {369, 276, 159, 119}},
    {"nvbamu",          {220, 230,  95, 143}},
    {"donghaiyandong",  {550, 258, 191,  89}},
    {"aolaiguo",        {410, 276, 223, 150}},
    {"datangguojing",   {377, 360, 351, 335}},
    {"chishuizhou",     {510, 303, 161,  95}},
    {"beijuluzhou",     {367, 276, 227, 169}},
};

// --- 操作间等待时间（毫秒）---
namespace Timing {
    constexpr int KEY_ACTION_DELAY_MS   = 100;   // 按键操作后等待
    constexpr int UI_UPDATE_DELAY_MS    = 1000;  // 普通 UI 更新等待
    constexpr int MAP_CHANGE_DELAY_MS   = 1500;  // 地图传送后等待（场景加载）
    constexpr int UI_CLICK_DELAY        = UI_UPDATE_DELAY_MS; // 兼容旧名
    constexpr int TRADE_CONFIRM_DELAY   = UI_UPDATE_DELAY_MS;
}

// --- 游戏窗口边框偏移（全屏窗口模式下的窗口装饰补偿）---
static constexpr int WINDOW_BORDER_OFFSET_X = -6;
static constexpr int WINDOW_BORDER_OFFSET_Y = -57;

// --- 交易智能比价阈值 ---
// 买入基准价：第一个商人价格 ≤ 此值时直接买，不再走去第二个商人比价
// 卖出利润率：卖价 / 买价 ≥ 此值时直接卖，不再比价（每种商品独立设置）
namespace TradeThreshold {
    constexpr int    PAPER_BUY_PRICE_MAX    = 2600;  // 纸钱买入基准价（低于此价直接买）
    constexpr int    OIL_BUY_PRICE_MAX      = 3600;  // 油买入基准价
    constexpr double PAPER_SELL_PROFIT_RATIO = 1.50;  // 纸钱卖出利润率阈值（1.15 = 15%）
    constexpr double OIL_SELL_PROFIT_RATIO   = 1.50;  // 油卖出利润率阈值（1.10 = 10%）
}

// ---------------------------------------------------------------------------
// 停止信号异常（内部使用，用于中断深层调用栈）
// ---------------------------------------------------------------------------
namespace {
    struct StopRequestedException final : std::exception {
        const char* what() const noexcept override { return "stop requested"; }
    };
    struct GoalReachedException final : std::exception {
        const char* what() const noexcept override { return "goal reached"; }
    };

    bool isStopRequested(const std::atomic_bool* flag) {
        return flag && flag->load(std::memory_order_relaxed);
    }
    [[noreturn]] void throwStop() { throw StopRequestedException{}; }
    void checkStop(const std::atomic_bool* flag) {
        if (isStopRequested(flag)) throwStop();
    }
    [[noreturn]] void throwGoalReached() { throw GoalReachedException{}; }

    // 输入驱动自检（仅初始化阶段调用）：
    // 1) 用 Win32 API 读取当前鼠标位置（before）
    // 2) 通过 IbInputSimulator 做一次相对移动
    // 3) 再次读取位置（after），校验方向与幅度是否符合预期
    // 4) 将鼠标移回原位，避免影响后续业务点击
    //
    // 设计目标：
    // - 提前发现“驱动已加载但实际不生效”的场景
    // - 将问题暴露在 init()，而不是跑商过程中才随机失败
    bool verifyIbMouseRelativeMove(std::string* reason) {
        POINT before{};
        if (!GetCursorPos(&before)) {
            if (reason) *reason = "GetCursorPos(before) failed";
            return false;
        }

        const int screenW = GetSystemMetrics(SM_CXSCREEN);
        const int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (screenW <= 0 || screenH <= 0) {
            if (reason) *reason = "GetSystemMetrics returned invalid screen size";
            return false;
        }

        int dx = (before.x <= screenW - 80) ? 40 : -40;
        int dy = (before.y <= screenH - 80) ? 28 : -28;
        if (dx == 0) dx = 40;
        if (dy == 0) dy = 28;

        // 发送一次相对移动，验证底层驱动链路是否可用。
        if (!IbSendMouseMove(static_cast<uint32_t>(dx), static_cast<uint32_t>(dy), Send::MoveMode::Relative)) {
            if (reason) *reason = "IbSendMouseMove(relative test move) failed";
            return false;
        }

        // 给系统一点时间刷新指针位置（不同虚拟化环境延迟不同）。
        Sleep(40);

        POINT after{};
        if (!GetCursorPos(&after)) {
            if (reason) *reason = "GetCursorPos(after) failed";
            return false;
        }

        const int movedX = after.x - before.x;
        const int movedY = after.y - before.y;

        // 校验策略：
        // - 方向一致（符号一致）
        // - 幅度至少达到目标值的一半，且不低于最小阈值
        //   （避免某些环境下被平滑/加速导致轻微衰减）
        const bool signXOk = (dx == 0) || (movedX == 0 ? false : ((movedX > 0) == (dx > 0)));
        const bool signYOk = (dy == 0) || (movedY == 0 ? false : ((movedY > 0) == (dy > 0)));
        const bool magXOk = std::abs(movedX) >= std::max(6, std::abs(dx) / 2);
        const bool magYOk = std::abs(movedY) >= std::max(6, std::abs(dy) / 2);
        const bool ok = signXOk && signYOk && magXOk && magYOk;

        // 回到原位置，避免影响后续点击逻辑。
        if (movedX != 0 || movedY != 0) {
            IbSendMouseMove(
                static_cast<uint32_t>(-movedX),
                static_cast<uint32_t>(-movedY),
                Send::MoveMode::Relative
            );
            Sleep(20);
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

}

// =============================================================================
// 构造 / 析构
// =============================================================================
TencentBot::TencentBot()
    : vision(screenCapture) {}  // VisionEngine 引用注入截图对象

TencentBot::~TencentBot() {
    IbSendDestroy();
    screenCapture.release();
}

void TencentBot::configureRunControl(std::atomic_bool* stopSignal, const std::string& checkpointFile) {
    stopSignal_ = stopSignal;
    if (!checkpointFile.empty()) checkpointFile_ = checkpointFile;
}

// =============================================================================
// init() — 初始化所有子系统
// =============================================================================
void TencentBot::init() {
    BOT_LOG("TencentBot", "========== 开始初始化 ==========");

    // --- 1. 遍历系统进程，找到游戏主进程和 UI 进程 ---
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

    // --- 2. 由当前内存后端初始化 mhmain.dll 基址 ---
    std::string init_err;
    if (!gameMemory.initialize_module_bases("mhmain.dll", &init_err)) {
        BOT_ERR("TencentBot", "初始化模块基址失败: " << init_err);
        throw std::runtime_error("initialize_module_bases failed");
    }

    // --- 3. 初始化鼠标模拟器 ---
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

    // --- 4. 初始化 AI 验证码/文字识别引擎 ---
    aiCaptcha = std::make_unique<CaptchaEngine>("http://127.0.0.1:8000");
    BOT_LOG("TencentBot", "CaptchaEngine 已连接 http://127.0.0.1:8000");

    // --- 5. 初始化屏幕捕获（DXGI）---
    if (!screenCapture.initByPid(static_cast<DWORD>(uiProcessPid))) {
        BOT_ERR("TencentBot", "DXGI 窗口捕获初始化失败 PID=" << uiProcessPid);
        throw std::runtime_error("DxgiWindowCapture init failed");
    } else {
        BOT_LOG("TencentBot", "DXGI 窗口捕获初始化成功");
    }

    // --- 6. 加载视觉识别模板 ---
    if (!vision.loadAllTemplates()) {
        BOT_ERR("TencentBot", "视觉模板加载失败");
        throw std::runtime_error("VisionEngine template load failed");
    }

    BOT_LOG("TencentBot", "========== 初始化完成 ==========");
}

// =============================================================================
// 底层鼠标移动 — 贝塞尔曲线轨迹 + 过冲拉回
// =============================================================================
namespace MouseCurve {
    constexpr double MOVE_SCALE           = 0.35;  // 游戏坐标差 → 鼠标位移的比例
    constexpr double OVERSHOOT_MIN        = 1.5;   // 过冲距离下限
    constexpr double OVERSHOOT_MAX        = 3.0;   // 过冲距离上限
    constexpr double PULLBACK_FACTOR      = 0.25;  // 拉回阶段每步的步长系数
    constexpr double ARRIVAL_THRESHOLD    = 2.0;   // 到达判定半径
    constexpr int    PULLBACK_MAX_ITERS   = 15;    // 拉回最大迭代次数
    constexpr double SHORT_MOVE_THRESHOLD = 6.0;   // 低于此距离不做过冲
}

void TencentBot::moveCharacterTo(int targetX, int targetY, int processIndex) {
    using namespace MouseCurve;

    RawCoord currentRaw = gameMemory.readPitPosRaw(static_cast<uint32_t>(processIndex));
    double startX = static_cast<double>(currentRaw.x);
    double startY = static_cast<double>(currentRaw.y);

    double deltaX    = targetX - startX;
    double deltaY    = targetY - startY;
    double totalDist = std::hypot(deltaX, deltaY);
    if (totalDist < 1.0) return; // 已经在目标位置

    // 线程安全的随机数生成
    auto randomInt = [](int lo, int hi) {
        static std::mt19937 gen(static_cast<unsigned>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        return std::uniform_int_distribution<>(lo, hi)(gen);
    };
    auto randomReal = [](double lo, double hi) {
        static std::mt19937 gen(static_cast<unsigned>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        return std::uniform_real_distribution<double>(lo, hi)(gen);
    };

    // --- 过冲目标点和三次贝塞尔控制点 ---
    double overshootX, overshootY;  // 过冲后的临时目标
    double ctrl1X, ctrl1Y;          // 第一控制点
    double ctrl2X, ctrl2Y;          // 第二控制点
    int    curveSteps;              // 贝塞尔插值步数

    if (totalDist < SHORT_MOVE_THRESHOLD) {
        // 短距离：直线到目标，不过冲
        overshootX = targetX;
        overshootY = targetY;
        ctrl1X = startX + deltaX * 0.33;
        ctrl1Y = startY + deltaY * 0.33;
        ctrl2X = startX + deltaX * 0.66;
        ctrl2Y = startY + deltaY * 0.66;
        curveSteps = std::max(4, static_cast<int>(totalDist));
    } else {
        // 长距离：加入过冲和曲线偏移，模拟人类手感
        double angle = std::atan2(deltaY, deltaX);
        double overshootDist = randomReal(OVERSHOOT_MIN, OVERSHOOT_MAX);
        overshootX = targetX + std::cos(angle) * overshootDist;
        overshootY = targetY + std::sin(angle) * overshootDist;

        // 在法线方向加入随机偏移（让轨迹带弧度）
        double sideSign    = (randomInt(0, 1) == 0) ? 1.0 : -1.0;
        double offsetScale = totalDist * randomReal(0.05, 0.1);
        double normalX     = -deltaY / totalDist;
        double normalY     =  deltaX / totalDist;

        ctrl1X = startX + (overshootX - startX) * 0.3 + normalX * offsetScale * sideSign;
        ctrl1Y = startY + (overshootY - startY) * 0.3 + normalY * offsetScale * sideSign;
        ctrl2X = startX + (overshootX - startX) * 0.7 + normalX * offsetScale * sideSign;
        ctrl2Y = startY + (overshootY - startY) * 0.7 + normalY * offsetScale * sideSign;
        curveSteps = std::clamp(static_cast<int>(totalDist / 3.5), 12, 50);
    }

    // --- 阶段1：沿贝塞尔曲线移动到过冲点 ---
    double errorAccumX = 0.0, errorAccumY = 0.0; // 亚像素误差累积
    for (int step = 1; step <= curveSteps; ++step) {
        RawCoord nowRaw = gameMemory.readPitPosRaw(static_cast<uint32_t>(processIndex));
        double curX = static_cast<double>(nowRaw.x);
        double curY = static_cast<double>(nowRaw.y);
        if (std::hypot(overshootX - curX, overshootY - curY) < 1.0) break;

        double t     = static_cast<double>(step) / curveSteps;
        double eased = t * (2.0 - t); // ease-out 缓动
        double inv   = 1.0 - eased;

        // 三次贝塞尔公式 B(t) = (1-t)^3*P0 + 3(1-t)^2*t*P1 + 3(1-t)*t^2*P2 + t^3*P3
        double bezX = inv*inv*inv * startX + 3*inv*inv*eased * ctrl1X
                      + 3*inv*eased*eased * ctrl2X + eased*eased*eased * overshootX;
        double bezY = inv*inv*inv * startY + 3*inv*inv*eased * ctrl1Y
                      + 3*inv*eased*eased * ctrl2Y + eased*eased*eased * overshootY;

        double moveX = (bezX - curX) * MOVE_SCALE + errorAccumX;
        double moveY = (bezY - curY) * MOVE_SCALE + errorAccumY;
        int intMoveX = static_cast<int>(std::round(moveX));
        int intMoveY = static_cast<int>(std::round(moveY));
        errorAccumX = moveX - intMoveX;
        errorAccumY = moveY - intMoveY;

        if (intMoveX != 0 || intMoveY != 0)
            IbSendMouseMove(intMoveX, intMoveY, Send::MoveMode::Relative);
        Sleep(1 + randomInt(0, 1));
    }

    // --- 阶段2：拉回到精确目标 ---
    Sleep(15 + randomInt(0, 14));
    double prevDistToTarget = 1e9;
    int    noImprovementCount = 0;

    for (int iter = 0; iter < PULLBACK_MAX_ITERS; ++iter) {
        RawCoord finalRaw = gameMemory.readPitPosRaw(static_cast<uint32_t>(processIndex));
        double curX = static_cast<double>(finalRaw.x);
        double curY = static_cast<double>(finalRaw.y);
        double diffX = targetX - curX;
        double diffY = targetY - curY;
        double distToTarget = std::hypot(diffX, diffY);

        if (distToTarget <= ARRIVAL_THRESHOLD) break;
        if (distToTarget >= prevDistToTarget) {
            if (++noImprovementCount >= 2) break;
        } else {
            noImprovementCount = 0;
        }
        prevDistToTarget = distToTarget;

        int corrX = static_cast<int>(std::round(diffX * MOVE_SCALE * PULLBACK_FACTOR));
        int corrY = static_cast<int>(std::round(diffY * MOVE_SCALE * PULLBACK_FACTOR));
        if (corrX == 0 && std::abs(diffX) >= 1.0) corrX = (diffX > 0 ? 1 : -1);
        if (corrY == 0 && std::abs(diffY) >= 1.0) corrY = (diffY > 0 ? 1 : -1);

        IbSendMouseMove(corrX, corrY, Send::MoveMode::Relative);
        Sleep(10 + randomInt(0, 4));
    }
}

void TencentBot::moveCharacterToOffset(int targetX, int targetY, int processIndex,
                                        int extraOffsetX, int extraOffsetY) {
    moveCharacterTo(targetX + extraOffsetX, targetY + extraOffsetY, processIndex);
}

// =============================================================================
// clickMapPosition() — 小地图坐标 → 屏幕像素 → 点击
// =============================================================================
void TencentBot::clickMapPosition(const std::string& mapName, int gameX, int gameY, int processIndex) {
    auto propIter = MAP_PROPERTIES.find(mapName);
    if (propIter == MAP_PROPERTIES.end()) {
        BOT_WARN("TencentBot", "clickMapPosition 未找到地图属性: " << mapName);
        return;
    }
    const MapProperties& mp = propIter->second;

    // 定位地图 UI 控件位置（最多重试 5 次）
    constexpr int MAP_UI_RETRY_WAIT_MS = 100;
    constexpr int MAP_UI_MAX_ATTEMPTS  = 5;

    MapUiLocation uiLoc = vision.locateMapUiOnScreen(mapName);
    for (int attempt = 1; !uiLoc.found && attempt < MAP_UI_MAX_ATTEMPTS; ++attempt) {
        BOT_WARN("TencentBot", "小地图 UI 定位失败: " << mapName << "，重试 " << attempt << "/" << (MAP_UI_MAX_ATTEMPTS-1));
        Sleep(MAP_UI_RETRY_WAIT_MS);
        uiLoc = vision.locateMapUiOnScreen(mapName);
    }
    if (!uiLoc.found) {
        BOT_ERR("TencentBot", "小地图 UI 最终未找到: " << mapName << "，跳过本次点击");
        return;
    }

    // 坐标换算：左下角为原点
    double originX = static_cast<double>(uiLoc.topLeftCorner.x);
    double originY = static_cast<double>(uiLoc.topLeftCorner.y + uiLoc.uiHeight);

    double pixelX = originX + (static_cast<double>(gameX) * mp.uiPixelWidth  / mp.gameCoordMaxX);
    double pixelY = originY - (static_cast<double>(gameY) * mp.uiPixelHeight / mp.gameCoordMaxY);

    clickUiElement(static_cast<int>(std::round(pixelX)),
                   static_cast<int>(std::round(pixelY)));
}

// =============================================================================
// clickUiElement() / clickNpcIfFound() / hideOtherPlayers()
// =============================================================================
void TencentBot::clickUiElement(int screenX, int screenY, int extraOffX, int extraOffY) {
    moveCharacterToOffset(screenX, screenY, 0,
                          WINDOW_BORDER_OFFSET_X + extraOffX,
                          WINDOW_BORDER_OFFSET_Y + extraOffY);
    IbSendMouseClick(Send::MouseButton::Left);
    Sleep(Timing::UI_UPDATE_DELAY_MS);
}

void TencentBot::hideOtherPlayers() {
    Send::KeyboardModifiers altKey{0};
    altKey.LAlt = true;
    IbSendKeybdDownUp('H', altKey);
    Sleep(Timing::KEY_ACTION_DELAY_MS);
    IbSendKeybdDown(VK_F9);
    IbSendKeybdUp(VK_F9);
    Sleep(Timing::KEY_ACTION_DELAY_MS);
}

bool TencentBot::clickNpcIfFound(const std::string& npcName, int clickOffsetX, int clickOffsetY) {
    hideOtherPlayers();
    auto matchPositions = vision.findNpcOnScreen(npcName);
    if (matchPositions.empty()) {
        BOT_WARN("TencentBot", "clickNpcIfFound: 未找到 NPC「" << npcName << "」");
        return false;
    }
    clickUiElement(matchPositions[0].x, matchPositions[0].y, clickOffsetX, clickOffsetY);
    return true;
}

// =============================================================================
// walkToPosition() — 打开小地图 → 点击 → 轮询等待角色到达
// =============================================================================
namespace WalkConfig {
    constexpr int TIMEOUT_MS       = 25000; // 25 秒超时
    constexpr int POLL_INTERVAL_MS = 500;   // 每 0.5 秒检查一次
    constexpr int ARRIVAL_RADIUS   = 4;     // 到达判定半径
}

void TencentBot::walkToPosition(const std::string& mapName, int targetX, int targetY) {
    BOT_LOG("TencentBot", "寻路: " << mapName << " → (" << targetX << "," << targetY << ")");
    checkStop(stopSignal_);

    // 按 Tab 打开小地图
    IbSendKeybdDown(VK_TAB); IbSendKeybdUp(VK_TAB);
    Sleep(Timing::KEY_ACTION_DELAY_MS);

    // 点击小地图上的目标位置
    clickMapPosition(mapName, targetX, targetY, 0);
    IbSendMouseClick(Send::MouseButton::Left);
    Sleep(Timing::UI_UPDATE_DELAY_MS);

    // 按 Tab 关闭小地图
    IbSendKeybdDown(VK_TAB); IbSendKeybdUp(VK_TAB);
    Sleep(Timing::KEY_ACTION_DELAY_MS);

    // 轮询角色坐标，等待到达
    int elapsedMs = 0;
    int stagnantIters = 0;
    double minDistance = 999999.0;
    int lastClickElapsedMs = 0;

    while (elapsedMs < WalkConfig::TIMEOUT_MS) {
        checkStop(stopSignal_);
        std::string curMap = vision.getCurrentMapName();
        GameCoord curPos = gameMemory.readRoleGameCoord(0, curMap);
        
        double dist = std::hypot(curPos.x - targetX, curPos.y - targetY);
        if (dist < WalkConfig::ARRIVAL_RADIUS)
            break;

        // 停滞检测：如果距离没有明显减小（允许 0.5 的波动），则增加停滞计数
        if (dist < minDistance - 0.5) {
            minDistance = dist;
            stagnantIters = 0;
        } else {
            stagnantIters++;
        }

        // 如果停滞超过 3 秒 (6次轮询)，或者距离上次点击超过 8 秒，则重新补点
        if (stagnantIters >= 6 || (elapsedMs - lastClickElapsedMs >= 8000)) {
            BOT_LOG("TencentBot", "检测到寻路停滞/超时，重新点击小地图 (" << stagnantIters << " iters)");
            
            IbSendKeybdDown(VK_TAB); IbSendKeybdUp(VK_TAB);
            Sleep(Timing::KEY_ACTION_DELAY_MS);
            clickMapPosition(mapName, targetX, targetY, 0);
            IbSendMouseClick(Send::MouseButton::Left);
            Sleep(Timing::UI_UPDATE_DELAY_MS);
            IbSendKeybdDown(VK_TAB); IbSendKeybdUp(VK_TAB);
            Sleep(Timing::KEY_ACTION_DELAY_MS);
            
            stagnantIters = 0;
            lastClickElapsedMs = elapsedMs;
        }

        Sleep(WalkConfig::POLL_INTERVAL_MS);
        elapsedMs += WalkConfig::POLL_INTERVAL_MS;
    }

    if (elapsedMs >= WalkConfig::TIMEOUT_MS) {
        BOT_WARN("TencentBot", "寻路最终超时: " << mapName << " (" << targetX << "," << targetY << ")");
    }
    Sleep(Timing::UI_UPDATE_DELAY_MS);
}

// =============================================================================
// tryNpcTeleport() / tryUiTeleport() — 传送辅助（含重试）
// =============================================================================
bool TencentBot::tryNpcTeleport(const std::string& npcName,
                                int npcClickOffX, int npcClickOffY,
                                int menuClickX, int menuClickY,
                                const std::string& expectedMap,
                                const std::string& retryWalkMap,
                                int retryWalkX, int retryWalkY, int maxRetries) {
    BOT_LOG("TencentBot", "传送(NPC): " << npcName << " → " << expectedMap);
    for (int attempt = 0; attempt < maxRetries; ++attempt) {
        checkStop(stopSignal_);
        BOT_LOG("TencentBot", "  尝试 " << (attempt+1) << "/" << maxRetries);
        if (attempt > 0) {
            hideOtherPlayers();
            Sleep(Timing::KEY_ACTION_DELAY_MS);
            walkToPosition(retryWalkMap, retryWalkX, retryWalkY);
            Sleep(Timing::UI_UPDATE_DELAY_MS);
        }
        if (clickNpcIfFound(npcName, npcClickOffX, npcClickOffY)) {
            checkStop(stopSignal_);
            clickUiElement(menuClickX, menuClickY);
            Sleep(Timing::MAP_CHANGE_DELAY_MS);
            std::string currentMap = vision.getCurrentMapName();
            if (currentMap == expectedMap) {
                BOT_LOG("TencentBot", "  到达 " << expectedMap);
                Sleep(Timing::KEY_ACTION_DELAY_MS);
                return true;
            }
            if (currentMap == "CaptureEmpty" && screenCapture.recreateIfNeeded())
                Sleep(Timing::UI_UPDATE_DELAY_MS);
        }
        Sleep(300);
    }
    BOT_ERR("TencentBot", "传送失败: " << expectedMap << " " << maxRetries << " 次后未到达");
    return false;
}

bool TencentBot::tryUiTeleport(int clickX, int clickY, int extraOffX, int extraOffY,
                               const std::string& expectedMap,
                               const std::string& retryWalkMap,
                               int retryWalkX, int retryWalkY, int maxRetries) {
    BOT_LOG("TencentBot", "传送(UI) → " << expectedMap);
    for (int attempt = 0; attempt < maxRetries; ++attempt) {
        checkStop(stopSignal_);
        BOT_LOG("TencentBot", "  尝试 " << (attempt+1) << "/" << maxRetries);
        if (attempt > 0) {
            hideOtherPlayers();
            Sleep(Timing::KEY_ACTION_DELAY_MS);
            walkToPosition(retryWalkMap, retryWalkX, retryWalkY);
            Sleep(Timing::UI_UPDATE_DELAY_MS);
        }
        clickUiElement(clickX, clickY, extraOffX, extraOffY);
        Sleep(Timing::MAP_CHANGE_DELAY_MS);
        std::string currentMap = vision.getCurrentMapName();
        if (currentMap == expectedMap) {
            BOT_LOG("TencentBot", "  到达 " << expectedMap);
            Sleep(Timing::KEY_ACTION_DELAY_MS);
            return true;
        }
        if (currentMap == "CaptureEmpty" && screenCapture.recreateIfNeeded())
            Sleep(Timing::UI_UPDATE_DELAY_MS);
        Sleep(300);
    }
    BOT_ERR("TencentBot", "传送失败: " << expectedMap << " " << maxRetries << " 次后未到达");
    return false;
}

// =============================================================================
// 各路线段函数（每个函数 = 一次地图间传送）
// =============================================================================

// 长安城 → 大唐国境：走到北门驿站老板(273,44)，NPC传送
void TencentBot::route_changan_to_datangguojing() {
    BOT_LOG("TencentBot", "路线: 长安城 → 大唐国境");
    walkToPosition("changancheng", 273, 44);
    tryNpcTeleport("驿站老板", 0, -65, 263, 473, "datangguojing", "changancheng", 273, 44);
}

// 大唐国境 → 地府：走到西南角传送门(46,324)，UI点击传送
void TencentBot::route_datangguojing_to_difu() {
    BOT_LOG("TencentBot", "路线: 大唐国境 → 地府");
    walkToPosition("datangguojing", 46, 324);
    tryUiTeleport(584, 75, 6, 57, "difu", "datangguojing", 46, 324);
}

// 地府 → 大唐国境：走到出口(141,4)，UI点击出地府
void TencentBot::route_leaveDisfu() {
    BOT_LOG("TencentBot", "路线: 地府 → 大唐国境（出地府）");
    walkToPosition("difu", 141, 4);
    clickUiElement(751, 709, 6, 57);
    Sleep(Timing::UI_UPDATE_DELAY_MS);
}

// 大唐国境 → 赤水洲：走到传送守卫(203,298)，NPC传送
void TencentBot::route_datangguojing_to_chishuizhou() {
    BOT_LOG("TencentBot", "路线: 大唐国境 → 赤水洲");
    walkToPosition("datangguojing", 203, 298);
    tryNpcTeleport("传送守卫", 0, -90, 251, 472, "chishuizhou", "datangguojing", 203, 298);
}

// 大唐国境 → 长安城：走到驿站老板(100,77)，NPC传送
void TencentBot::route_datangguojing_to_changancheng() {
    BOT_LOG("TencentBot", "路线: 大唐国境 → 长安城");
    walkToPosition("datangguojing", 91, 255);
    tryNpcTeleport("驿站老板", 0, -50, 263, 473, "changancheng", "datangguojing", 91, 255);
}

// 赤水洲 → 女魃墓
void TencentBot::route_chishuizhou_to_nvbamu() {
    BOT_LOG("TencentBot", "路线: 赤水洲 → 女魃墓");
    walkToPosition("chishuizhou", 100, 77);
    tryUiTeleport(547, 295, 6, 57, "nvbamu", "chishuizhou", 100, 77);
}

// 女魃墓 → 东海岩洞
void TencentBot::route_nvbamu_to_donghaiyandong() {
    BOT_LOG("TencentBot", "路线: 女魃墓 → 东海岩洞");
    walkToPosition("nvbamu", 13, 5);
    tryUiTeleport(124, 600, 6, 57, "donghaiyandong", "nvbamu", 13, 5);
}

// 东海岩洞 → 东海湾
void TencentBot::route_donghaiyandong_to_donghaiwan() {
    BOT_LOG("TencentBot", "路线: 东海岩洞 → 东海湾");
    walkToPosition("donghaiyandong", 85, 5);
    tryUiTeleport(384, 705, 6, 57, "donghaiwan", "donghaiyandong", 85, 5);
}

// 东海湾 → 傲来国
void TencentBot::route_donghaiwan_to_aolaiguo() {
    BOT_LOG("TencentBot", "路线: 东海湾 → 傲来国");
    walkToPosition("donghaiwan", 68, 18);
    Sleep(Timing::UI_UPDATE_DELAY_MS);
    tryNpcTeleport("传送傲来", -5, -65, 256, 471, "aolaiguo", "donghaiwan", 68, 18);
}

// 傲来国 → 花果山
void TencentBot::route_aolaiguo_to_huaguoshan() {
    BOT_LOG("TencentBot", "路线: 傲来国 → 花果山");
    walkToPosition("aolaiguo", 210, 141);
    tryUiTeleport(871, 88, 6, 57, "huaguoshan", "aolaiguo", 210, 141);
}

// 花果山 → 北俱泸州
void TencentBot::route_huaguoshan_to_beijuluzhou() {
    BOT_LOG("TencentBot", "路线: 花果山 → 北俱泸州");
    walkToPosition("huaguoshan", 19, 97);
    Sleep(Timing::UI_UPDATE_DELAY_MS);
    tryNpcTeleport("传送北俱", -5, -50, 269, 471, "beijuluzhou", "huaguoshan", 19, 97);
}

// 北俱泸州 → 长安城（驿站老板传送）
void TencentBot::route_beijuluzhou_to_changan() {
    BOT_LOG("TencentBot", "路线: 北俱泸州 → 长安城");
    walkToPosition("beijuluzhou", 42, 117);
    Sleep(Timing::UI_UPDATE_DELAY_MS);
    tryNpcTeleport("驿站老板", 0, -60, 269, 471, "changancheng", "beijuluzhou", 42, 117);
}

// 长安城 → 帮派（帮派主管 NPC + 多步 UI 操作）
void TencentBot::route_changan_to_bangpai() {
    BOT_LOG("TencentBot", "路线: 长安城 → 帮派");
    
    for (int retry = 1; retry <= 3; ++retry) {
        checkStop(stopSignal_);
        
        walkToPosition("changancheng", 386, 260);
        Sleep(Timing::UI_UPDATE_DELAY_MS);
        if (!clickNpcIfFound("帮派主管", 0, -50)) {
            BOT_WARN("TencentBot", "未找到帮派主管，尝试 " << retry << "/3    ");
            continue;
        }
        //点击回帮派选项
        clickUiElement(256, 473);
        Sleep(Timing::UI_UPDATE_DELAY_MS);
        //进入金库
        clickUiElement(1009, 352, 6, 57);
        Sleep(Timing::UI_UPDATE_DELAY_MS);
        Sleep(2000);
        clickUiElement(831, 338, 6, 57);
        Sleep(Timing::UI_UPDATE_DELAY_MS);
        Sleep(2000);
        //到达金库指定点
        clickUiElement(336, 246, 6, 57);
        Sleep(Timing::UI_UPDATE_DELAY_MS);
        Sleep(2000);

        std::string curMapName = vision.getCurrentMapName();
        if (curMapName == "bangpai" || curMapName == "jinku") {
            BOT_LOG("TencentBot", "成功进入帮派/金库 (当前地图: " << curMapName << ")");
            return;
        }
        
        BOT_WARN("TencentBot", "未能进入帮派，重试 " << retry << "/3");
        Sleep(Timing::UI_UPDATE_DELAY_MS);
    }

    BOT_ERR("TencentBot", "多次尝试进入帮派失败，请手动检查");
}

// 帮派 → 长安城（出帮派 UI 按钮序列）
// 帮派 → 长安城（智能判断当前位置，仅依赖 UI 识别）
void TencentBot::route_leaveBangpai() {
    BOT_LOG("TencentBot", "出帮派 → 长安城");

    // 只能通过 UI 匹配来判断是否在金库或帮派，内存坐标无效
    std::string curMap = vision.getCurrentMapName();
    
    // 如果识别到金库 UI，点击离开
    if (curMap == "jinku") {
        BOT_LOG("TencentBot", "识别到金库 UI，点击离开...");
        clickUiElement(824, 708, 6, 57);
        Sleep(2000); // 走到金库外
        
        // 刷新地图识别
        Sleep(Timing::UI_UPDATE_DELAY_MS);
        curMap = vision.getCurrentMapName();
    }

    // 如果识别到帮派 UI，点击离开
    if (curMap == "bangpai") {
        BOT_LOG("TencentBot", "识别到帮派 UI，点击离开...");
        clickUiElement(7, 521, 6, 57);
        Sleep(2000); // 走到帮派外
        Sleep(Timing::UI_UPDATE_DELAY_MS);
    }

    // 如果是 Unknown，尝试盲点（假设在金库或帮派）
    if (curMap == "Unknown") {
        BOT_WARN("TencentBot", "当前地图 Unknown，尝试按金库/帮派顺序盲点离开...");
        clickUiElement(824, 708, 6, 57); // 试金库出口
        Sleep(1000);
        clickUiElement(7, 521, 6, 57);   // 试帮派出口
        Sleep(Timing::UI_UPDATE_DELAY_MS);
    }
}

// =============================================================================
// 商人快捷走位
// =============================================================================
void TencentBot::walkToDifuLowerMerchant()  { walkToPosition("difu", 92, 10); }
void TencentBot::walkToDifuUpperMerchant()  { walkToPosition("difu", 70, 44); }
void TencentBot::walkToBeixuUpperMerchant() { walkToPosition("beijuluzhou", 164, 112); }
void TencentBot::walkToBeixuLowerMerchant() { walkToPosition("beijuluzhou", 160, 40); }

// =============================================================================
// 断点续跑恢复路线
// =============================================================================
void TencentBot::resumeRoute_leaveBangpaiToDifu() {
    std::string currentMap = vision.getCurrentMapName();
    BOT_LOG("TencentBot", "恢复路线: 帮派→地府, 当前地图=" << currentMap);

    if (currentMap == "bangpai") {
        route_leaveBangpai();
        route_changan_to_datangguojing();
        route_datangguojing_to_difu();
    } else if (currentMap == "changancheng") {
        route_changan_to_datangguojing();
        route_datangguojing_to_difu();
    } else if (currentMap == "datangguojing") {
        route_datangguojing_to_difu();
    } else if (currentMap == "difu") {
        BOT_LOG("TencentBot", "已在地府，跳过恢复路线");
    } else {
        BOT_ERR("TencentBot", "无法恢复 leave_bangpai_to_difu，当前地图=" << currentMap);
    }
}

void TencentBot::resumeRoute_travelToBeixu() {
    std::string currentMap = vision.getCurrentMapName();
    BOT_LOG("TencentBot", "恢复路线: 地府→北俱, 当前地图=" << currentMap);

    // 路线链：地府→大唐→赤水洲→女魃墓→东海岩洞→东海湾→傲来→花果山→北俱
    // 根据当前地图，从对应节点往后执行
    if (currentMap == "difu")            { route_leaveDisfu(); currentMap = "datangguojing"; }
    if (currentMap == "datangguojing")   { route_datangguojing_to_chishuizhou(); currentMap = "chishuizhou"; }
    if (currentMap == "chishuizhou")     { route_chishuizhou_to_nvbamu(); currentMap = "nvbamu"; }
    if (currentMap == "nvbamu")          { route_nvbamu_to_donghaiyandong(); currentMap = "donghaiyandong"; }
    if (currentMap == "donghaiyandong")  { route_donghaiyandong_to_donghaiwan(); currentMap = "donghaiwan"; }
    if (currentMap == "donghaiwan")      { route_donghaiwan_to_aolaiguo(); currentMap = "aolaiguo"; }
    if (currentMap == "aolaiguo")        { route_aolaiguo_to_huaguoshan(); currentMap = "huaguoshan"; }
    if (currentMap == "huaguoshan")      { route_huaguoshan_to_beijuluzhou(); currentMap = "beijuluzhou"; }

    if (currentMap == "beijuluzhou") {
        BOT_LOG("TencentBot", "已到达北俱泸州");
    } else {
        BOT_ERR("TencentBot", "无法恢复 travel_to_beiju，当前地图=" << currentMap);
    }
}

// =============================================================================
// Part 2: 交易面板操作 + 完整跑商循环
// =============================================================================

// =============================================================================
// readItemPrice() — 在交易面板里找到物品并读取价格
// tradeMode: 0=买入（左侧面板宽 517px），1=卖出（右侧面板宽 294px，起始x=517）
// =============================================================================
int TencentBot::readItemPrice(int tradeMode, const std::string& itemName) {
    // 根据买/卖模式确定搜索区域
    int searchRegionX = (tradeMode == 0) ? 0 : 517;
    int searchRegionW = (tradeMode == 0) ? 517 : 294;

    // --- 第一阶段：在交易面板中定位物品图标 ---
    vision.captureToBuffer();
    if (vision.imageBuffer.empty() || vision.frameW <= 0 || vision.frameH <= 0) return -1;

    cv::Mat fullScreenImg(vision.frameH, vision.frameW, CV_8UC4, vision.imageBuffer.data());
    if (searchRegionX >= vision.frameW) return -2;
    searchRegionW = std::min(searchRegionW, vision.frameW - searchRegionX);
    cv::Rect searchRect(searchRegionX, 0, searchRegionW, vision.frameH);
    cv::Mat searchRegionImg = fullScreenImg(searchRect).clone();

    std::vector<uint8_t> searchImgData(searchRegionImg.data,
        searchRegionImg.data + (searchRegionImg.total() * searchRegionImg.elemSize()));
    auto detectedPositions = aiCaptcha->detectObject(
        searchRegionImg.cols, searchRegionImg.rows, searchImgData, itemName);

    if (detectedPositions.empty()) {
        // 兜底：AI 检测失败时用本地模板匹配
        Point2D localMatch = vision.findNpcInScreenRegion(itemName, searchRegionX, 0, searchRegionW, vision.frameH);
        if (localMatch.score <= 0) return -1;
        clickUiElement(localMatch.x, localMatch.y);
    } else {
        clickUiElement(detectedPositions[0].x + searchRegionX, detectedPositions[0].y);
    }

    // 鼠标移走避免遮挡价格区域
    moveCharacterTo(200, 200, 0);
    Sleep(Timing::UI_UPDATE_DELAY_MS);

    // --- 第二阶段：截图识别价格数字 ---
    vision.captureToBuffer();
    cv::Mat freshImg(vision.frameH, vision.frameW, CV_8UC4, vision.imageBuffer.data());

    int priceRoiX = (tradeMode == 0) ? 386 : 657;
    if (priceRoiX + 125 > vision.frameW || 500 + 32 > vision.frameH) return -2;

    cv::Rect priceRect(priceRoiX, 500, 125, 32);
    cv::Mat priceRegionImg = freshImg(priceRect).clone();
    std::vector<uint8_t> priceImgData(priceRegionImg.data,
        priceRegionImg.data + (priceRegionImg.total() * priceRegionImg.elemSize()));
    std::string recognizedText = aiCaptcha->recognizeNumber(
        priceRegionImg.cols, priceRegionImg.rows, priceImgData);

    try {
        if (recognizedText.empty()) return 0;
        return std::stoi(recognizedText);
    } catch (...) {
        return -3;
    }
}

// =============================================================================
// readCurrentMoney() — 读取当前银票总额
// =============================================================================
int TencentBot::readCurrentMoney() {
    moveCharacterTo(200, 70, 0); // 鼠标移走避免遮挡
    Sleep(Timing::KEY_ACTION_DELAY_MS);

    vision.captureToBuffer();
    if (vision.imageBuffer.empty()) return -1;

    cv::Mat fullImg(vision.frameH, vision.frameW, CV_8UC4, vision.imageBuffer.data());
    cv::Rect moneyRect(381, 589, 120, 40);
    moneyRect &= cv::Rect(0, 0, fullImg.cols, fullImg.rows);
    if (moneyRect.width <= 0 || moneyRect.height <= 0) return -4;

    cv::Mat moneyRegionImg = fullImg(moneyRect).clone();
    std::vector<uint8_t> moneyImgData(moneyRegionImg.data,
        moneyRegionImg.data + (moneyRegionImg.total() * moneyRegionImg.elemSize()));
    std::string recognizedText = aiCaptcha->recognizeNumber(
        moneyRegionImg.cols, moneyRegionImg.rows, moneyImgData);

    try {
        if (recognizedText.empty()) return -2;
        return std::stoi(recognizedText);
    } catch (...) {
        return -3;
    }
}

// =============================================================================
// queryNpcBuyPrice / queryNpcSalePrice — 查询价格后关闭面板
// =============================================================================
int TencentBot::queryNpcBuyPrice(const std::string& npcName, const std::string& itemName) {
    if (!clickNpcIfFound(npcName, 0, -40)) return -1;
    clickUiElement(242, 473);
    if (!waitForTradePanel()) return -1;

    int price = readItemPrice(0, itemName);
    auto cancelHits = vision.findNpcOnScreen("取消");
    if (!cancelHits.empty()) clickUiElement(cancelHits[0].x, cancelHits[0].y);
    return price;
}

int TencentBot::queryNpcSalePrice(const std::string& npcName, const std::string& itemName) {
    if (!clickNpcIfFound(npcName, 0, -40)) return -1;
    clickUiElement(242, 473);
    if (!waitForTradePanel()) return -1;

    int price = readItemPrice(1, itemName);
    auto cancelHits = vision.findNpcOnScreen("取消");
    if (!cancelHits.empty()) clickUiElement(cancelHits[0].x, cancelHits[0].y);
    return price;
}

// =============================================================================
// buyItemFromNpc() — 完整买入流程
// =============================================================================
// =============================================================================
// waitForTradePanel() — 等待交易面板打开（检测 UI 标题等）
// =============================================================================
bool TencentBot::waitForTradePanel() {
    // 最多等待 3 秒 (15 * 200ms)
    for (int i = 0; i < 15; ++i) {
        checkStop(stopSignal_);
        auto hits = vision.findNpcOnScreen("交易面板");
        if (!hits.empty()) return true;
        Sleep(200);
    }
    BOT_WARN("TencentBot", "等待交易面板超时");
    return false;
}

// =============================================================================
// buyItemFromNpc() — 点击 NPC → 打开买入面板 → 找物品 → 最大数量 → 确定
// =============================================================================
bool TencentBot::buyItemFromNpc(const std::string& npcName, const std::string& itemName) {
    checkStop(stopSignal_);
    if (!clickNpcIfFound(npcName, 0, -40)) return false;
    clickUiElement(242, 473); // 点击“交易”按钮
    
    // 等待面板打开
    if (!waitForTradePanel()) return false;

    auto itemHits = vision.findNpcOnScreen(itemName);
    if (itemHits.empty()) return false;
    clickUiElement(itemHits[0].x, itemHits[0].y);

    // 点击"最大数量"按钮（左侧面板范围 0~517）
    Point2D maxQtyBtn = vision.findNpcInScreenRegion("最大数量", 0, 0, 517, 718);
    if (maxQtyBtn.score <= 0) return false;
    clickUiElement(maxQtyBtn.x, maxQtyBtn.y);

    // 点击"确定"按钮
    Point2D confirmBtn = vision.findNpcInScreenRegion("确定", 0, 0, 517, 718);
    if (confirmBtn.score <= 0) return false;
    clickUiElement(confirmBtn.x, confirmBtn.y);

    clickUiElement(858, 430); // 关闭提示弹窗

    auto cancelHits = vision.findNpcOnScreen("取消");
    if (!cancelHits.empty()) clickUiElement(cancelHits[0].x, cancelHits[0].y);

    BOT_LOG("TencentBot", "买入 " << itemName << " 完成");
    return true;
}

// =============================================================================
// sellItemToNpc() — 完整卖出流程（通过检测物品图标消失判断成功）
// =============================================================================
int TencentBot::sellItemToNpc(const std::string& npcName, const std::string& itemName) {
    checkStop(stopSignal_);
    constexpr int VERIFY_HALF_SIZE = 40; // 验证区域半径

    for (int attempt = 0; attempt < 2; ++attempt) {
        checkStop(stopSignal_);
        if (attempt > 0) {
            BOT_WARN("TencentBot", "卖出 " << itemName << " 重试 (" << (attempt+1) << "/2)");
            auto cancelHits = vision.findNpcOnScreen("取消");
            if (!cancelHits.empty()) clickUiElement(cancelHits[0].x, cancelHits[0].y);
            Sleep(Timing::UI_CLICK_DELAY);
        }

        // 打开交易面板
        if (!clickNpcIfFound(npcName, 0, -40)) continue;
        clickUiElement(242, 473);
        
        // 等待面板打开
        if (!waitForTradePanel()) {
             auto cancelHits = vision.findNpcOnScreen("取消");
             if (!cancelHits.empty()) clickUiElement(cancelHits[0].x, cancelHits[0].y);
             continue;
        }

        // 在面板中找到要卖的物品
        auto itemHits = vision.findNpcOnScreen(itemName);
        if (itemHits.empty()) {
            BOT_WARN("TencentBot", "卖出列表中未找到 " << itemName);
            continue;
        }
        int itemCenterX = itemHits[0].x;
        int itemCenterY = itemHits[0].y;
        clickUiElement(itemCenterX, itemCenterY);

        // 点击"最大数量"和"确定"（右侧面板范围 517~811）
        checkStop(stopSignal_);
        moveCharacterTo(200, 70, 0);
        Point2D maxQtyBtn = vision.findNpcInScreenRegion("最大数量", 517, 0, 294, 718);
        if (maxQtyBtn.score <= 0) { BOT_WARN("TencentBot", "未找到\"最大数量\"按钮"); continue; }
        clickUiElement(maxQtyBtn.x, maxQtyBtn.y);

        Point2D confirmBtn = vision.findNpcInScreenRegion("确定", 517, 0, 294, 718);
        if (confirmBtn.score <= 0) { BOT_WARN("TencentBot", "未找到\"确定\"按钮"); continue; }
        clickUiElement(confirmBtn.x, confirmBtn.y);
        Sleep(Timing::TRADE_CONFIRM_DELAY);

        // 验证物品是否已消失（在原位置的小区域再匹配一次）
        int verifyX = std::max(0, itemCenterX - VERIFY_HALF_SIZE);
        int verifyY = std::max(0, itemCenterY - VERIFY_HALF_SIZE);
        vision.captureToBuffer();
        Point2D verifyHit = vision.findNpcInScreenRegion(
            itemName, verifyX, verifyY, VERIFY_HALF_SIZE * 2, VERIFY_HALF_SIZE * 2);
        bool itemGone = (verifyHit.score <= 0);

        if (itemGone) {
            BOT_LOG("TencentBot", "卖出 " << itemName << " 成功（物品格子已清空）");
            // 【改进】面板仍然打开，直接读取银票总额，省去再次点击 NPC
            int currentMoney = readCurrentMoney();
            BOT_LOG("TencentBot", "卖出后银票=" << currentMoney);
            // 关闭交易面板
            auto cancelHits = vision.findNpcOnScreen("取消");
            if (!cancelHits.empty()) clickUiElement(cancelHits[0].x, cancelHits[0].y);
            return currentMoney; // ≥0 表示卖出成功并返回银票数
        }

        BOT_WARN("TencentBot", "卖出 " << itemName << " 后仍检测到物品图标，视为失败");
        auto cancelHits = vision.findNpcOnScreen("取消");
        if (!cancelHits.empty()) clickUiElement(cancelHits[0].x, cancelHits[0].y);
    }

    BOT_ERR("TencentBot", "卖出 " << itemName << " 失败（多次尝试后格子里仍有该物品）");
    auto cancelHits = vision.findNpcOnScreen("取消");
    if (!cancelHits.empty()) clickUiElement(cancelHits[0].x, cancelHits[0].y);
    return -1;
}

// =============================================================================
// runTradingRoute() — 完整跑商循环（含断点续跑 + 达标回帮派）
// =============================================================================
void TencentBot::runTradingRoute() {
    BOT_LOG("TencentBot", "========== 跑商开始 ==========");

    // --- 加载断点 ---
    CheckpointStore store(checkpointFile_);
    TradingCheckpoint ck;
    std::string ckError;
    if (store.load(ck, &ckError)) {
        BOT_LOG("TencentBot", "续跑: next_op=" << ck.next_op
                << " last=\"" << ck.last_op_name << "\" cycle=" << ck.cycle
                << " target=" << ck.target_money);
    } else {
        if (!ckError.empty()) BOT_WARN("TencentBot", "checkpoint 读取失败，从头开始: " << ckError);
        ck = TradingCheckpoint{};
        (void)store.save(ck);
    }

    if (screenCapture.recreateIfNeeded()) {
        BOT_LOG("TencentBot", "窗口捕获已恢复");
        Sleep(500);
    }

    // --- 跟踪实际买入价（用于卖出时计算利润率）---
    int lastPaperBuyPrice = 0;  // 上次买纸钱的实际价格
    int lastOilBuyPrice   = 0;  // 上次买油的实际价格

    // --- 银票检测 & 达标回帮派 ---
    auto readMoneyFromPanel = [&](const std::string& npcName) -> int {
        auto ensureNearNpc = [&]() {
            if (npcName == "地府商人")      walkToDifuLowerMerchant();
            else if (npcName == "地府货商") walkToDifuUpperMerchant();
            else if (npcName == "北俱商人") walkToBeixuLowerMerchant();
            else if (npcName == "北俱货商") walkToBeixuUpperMerchant();
        };
        for (int trial = 0; trial < 2; ++trial) {
            checkStop(stopSignal_);
            if (trial > 0) { ensureNearNpc(); Sleep(Timing::UI_UPDATE_DELAY_MS); }
            if (!clickNpcIfFound(npcName, 0, -40)) continue;
            clickUiElement(242, 473);
            moveCharacterTo(200, 70, 0);
            Sleep(Timing::UI_UPDATE_DELAY_MS);
            int money = readCurrentMoney();
            auto cancelHits = vision.findNpcOnScreen("取消");
            if (!cancelHits.empty()) clickUiElement(cancelHits[0].x, cancelHits[0].y);
            if (money >= 0) return money;
        }
        return -10;
    };

    auto step6_returnAndSubmit = [&]() {
        BOT_LOG("TencentBot", "[6/6] 达标回帮派 + 提交银票");
        checkStop(stopSignal_);

        std::string curMap = vision.getCurrentMapName();
        if (curMap == "beijuluzhou")   { route_beijuluzhou_to_changan(); curMap = vision.getCurrentMapName(); }
        if (curMap == "difu")          { route_leaveDisfu(); curMap = vision.getCurrentMapName(); }
        if (curMap == "datangguojing") { route_datangguojing_to_changancheng(); curMap = vision.getCurrentMapName(); }
        
        if (curMap == "changancheng") {
            route_changan_to_bangpai();
            curMap = vision.getCurrentMapName();
        }

        if (curMap == "bangpai" || curMap == "jinku") {
            process_idiom_verify(); // 提交银票并处理可能出现的成语验证码
            BOT_LOG("TencentBot", "已返回帮派并提交银票，跑商完成");
            (void)store.clear();
            throwGoalReached();
        }

        BOT_ERR("TencentBot", "当前地图不在回城路径上(" << curMap << ")，请手动回城完成最后提交");
        ck.last_op_name = "return_path_unexpected_map";
        (void)store.save(ck);
    };

    auto tryReturnToGangIfReached = [&](const char* afterWhat, int money) {
        checkStop(stopSignal_);
        BOT_LOG("TencentBot", "(" << afterWhat << ") 银票=" << money << " 目标=" << ck.target_money);
        if (!(money >= 0 && money >= ck.target_money)) return;

        BOT_LOG("TencentBot", "已达标，记录断点并跳转到回帮步骤");
        ck.is_goal_reached = true;
        ck.last_op_name = std::string("goal_reached_after_") + afterWhat;
        (void)store.save(ck); // 保存关键的 is_goal_reached 状态

        // 立即执行回帮逻辑
        step6_returnAndSubmit();
    };

    // --- 6 个操作步骤 ---
    auto step0_leaveBangpaiToDifu = [&]() {
        BOT_LOG("TencentBot", "[1/6] 出帮派 → 长安 → 大唐国境 → 地府");
        checkStop(stopSignal_);
        route_leaveBangpai();
        route_changan_to_datangguojing();
        route_datangguojing_to_difu();
    };

    auto step1_difuBuyPaper = [&]() {
        BOT_LOG("TencentBot", "[2/6] 地府买纸钱");
        
        while (true) {
            checkStop(stopSignal_);
            std::string bestMerchant = ck.preferred_difu_merchant;
            int price1 = -1, price2 = -1;

            if (!bestMerchant.empty()) {
                BOT_LOG("TencentBot", "  使用首选地府商人: " << bestMerchant);
            } else {
                 // 首次运行，需要比价
                 walkToDifuUpperMerchant();
                 price1 = queryNpcBuyPrice("地府货商", "纸钱");
                 if (price1 < 0) { Sleep(Timing::UI_UPDATE_DELAY_MS); walkToDifuUpperMerchant(); price1 = queryNpcBuyPrice("地府货商", "纸钱"); }
                 BOT_LOG("TencentBot", "  地府货商 纸钱买价=" << (price1 > 0 ? std::to_string(price1) : "失败"));

                 if (price1 > 0 && price1 <= TradeThreshold::PAPER_BUY_PRICE_MAX) {
                     bestMerchant = "地府货商";
                 } else {
                     walkToDifuLowerMerchant();
                     price2 = queryNpcBuyPrice("地府商人", "纸钱");
                     if (price2 < 0) { Sleep(Timing::UI_UPDATE_DELAY_MS); walkToDifuLowerMerchant(); price2 = queryNpcBuyPrice("地府商人", "纸钱"); }
                     BOT_LOG("TencentBot", "  地府商人 纸钱买价=" << (price2 > 0 ? std::to_string(price2) : "失败"));

                     if (price1 > 0 && (price2 < 0 || price1 <= price2)) bestMerchant = "地府货商";
                     else bestMerchant = "地府商人";
                 }
                 ck.preferred_difu_merchant = bestMerchant;
                 (void)store.save(ck);
            }

            // --- 执行购买 ---
            std::string otherMerchant = (bestMerchant == "地府货商") ? "地府商人" : "地府货商";
            
            // 尝试首选
            if (bestMerchant == "地府货商") walkToDifuUpperMerchant(); else walkToDifuLowerMerchant();
            if (buyItemFromNpc(bestMerchant, "纸钱")) {
                lastPaperBuyPrice = (price1 > 0) ? price1 : (price2 > 0 ? price2 : 2700);
                BOT_LOG("TencentBot", "  购买成功，价格(估)=" << lastPaperBuyPrice);
                return;
            }

            // 首选失败（可能卖完了），尝试另一个
            BOT_WARN("TencentBot", "首选商人 " << bestMerchant << " 购买失败，尝试备选 " << otherMerchant);
            if (otherMerchant == "地府货商") walkToDifuUpperMerchant(); else walkToDifuLowerMerchant();
            if (buyItemFromNpc(otherMerchant, "纸钱")) {
                lastPaperBuyPrice = (price2 > 0) ? price2 : (price1 > 0 ? price1 : 2700);
                BOT_LOG("TencentBot", "  备选购买成功，价格(估)=" << lastPaperBuyPrice);
                // 既然备选买到了，以后就先看备选
                ck.preferred_difu_merchant = otherMerchant;
                (void)store.save(ck);
                return;
            }

            BOT_WARN("TencentBot", "地府纸钱可能全卖完了，等待 30 秒重试...");
            Sleep(30000);
        }
    };

    auto step2_travelToBeiju = [&]() {
        BOT_LOG("TencentBot", "[3/6] 地府 → … → 北俱泸州（8段传送）");
        checkStop(stopSignal_);
        route_leaveDisfu();
        route_datangguojing_to_chishuizhou();
        route_chishuizhou_to_nvbamu();
        route_nvbamu_to_donghaiyandong();
        route_donghaiyandong_to_donghaiwan();
        route_donghaiwan_to_aolaiguo();
        route_aolaiguo_to_huaguoshan();
        route_huaguoshan_to_beijuluzhou();
    };

    auto step3_beixuSellPaperBuyOil = [&]() {
        BOT_LOG("TencentBot", "[4/6] 北俱: 卖纸钱 + 买油");
        checkStop(stopSignal_);

        // 1. 必查两个商人的卖价（纸钱）
        walkToBeixuUpperMerchant();
        int salePrice1 = queryNpcSalePrice("北俱货商", "纸钱");
        if (salePrice1 < 0) { Sleep(Timing::UI_UPDATE_DELAY_MS); walkToBeixuUpperMerchant(); salePrice1 = queryNpcSalePrice("北俱货商", "纸钱"); }
        BOT_LOG("TencentBot", "  北俱货商 纸钱收购价=" << salePrice1);

        checkStop(stopSignal_);
        walkToBeixuLowerMerchant();
        int salePrice2 = queryNpcSalePrice("北俱商人", "纸钱");
        if (salePrice2 < 0) { Sleep(Timing::UI_UPDATE_DELAY_MS); walkToBeixuLowerMerchant(); salePrice2 = queryNpcSalePrice("北俱商人", "纸钱"); }
        BOT_LOG("TencentBot", "  北俱商人 纸钱收购价=" << salePrice2);

        // 2. 决策：谁卖价高给谁卖，然后去另一个买油
        std::string sellTarget, buyTarget;
        if (salePrice1 >= salePrice2) {
             sellTarget = "北俱货商"; // Upper
             buyTarget  = "北俱商人"; // Lower
        } else {
             sellTarget = "北俱商人"; // Lower
             buyTarget  = "北俱货商"; // Upper
        }

        BOT_LOG("TencentBot", "  决策: 卖给 " << sellTarget << "，从 " << buyTarget << " 买油");

        // 3. 执行卖出
        if (sellTarget == "北俱货商") walkToBeixuUpperMerchant();
        else walkToBeixuLowerMerchant();
        
        int paperMoney = sellItemToNpc(sellTarget, "纸钱");
        tryReturnToGangIfReached("beiju_sale_paper", paperMoney);

        // 4. 执行买入（带缺货重试）
        while (true) {
            checkStop(stopSignal_);
            if (buyTarget == "北俱货商") walkToBeixuUpperMerchant();
            else walkToBeixuLowerMerchant();

            BOT_LOG("TencentBot", "  尝试向 " << buyTarget << " 买油");
            if (buyItemFromNpc(buyTarget, "油")) {
                lastOilBuyPrice = 3500; // 估值
                return;
            }

            // 首选失败，尝试另一个
            std::string altTarget = (buyTarget == "北俱货商") ? "北俱商人" : "北俱货商";
            BOT_WARN("TencentBot", "首选买油商人 " << buyTarget << " 缺货，尝试备选 " << altTarget);
            if (altTarget == "北俱货商") walkToBeixuUpperMerchant(); else walkToBeixuLowerMerchant();
            if (buyItemFromNpc(altTarget, "油")) {
                lastOilBuyPrice = 3500;
                return;
            }

            BOT_WARN("TencentBot", "北俱油可能全卖完了，等待 30 秒重试...");
            Sleep(30000);
        }
    };

    auto step4_returnToDifu = [&]() {
        BOT_LOG("TencentBot", "[5/6] 北俱 → 长安 → 大唐国境 → 地府");
        checkStop(stopSignal_);
        route_beijuluzhou_to_changan();
        route_changan_to_datangguojing();
        route_datangguojing_to_difu();
    };

    auto step5_difuSellOil = [&]() {
        BOT_LOG("TencentBot", "[6/6] 地府卖油");
        checkStop(stopSignal_);

        // 1. 必查两个商人的卖价（油）
        walkToDifuUpperMerchant();
        int oilSalePrice1 = queryNpcSalePrice("地府货商", "油");
        if (oilSalePrice1 < 0) { Sleep(Timing::UI_UPDATE_DELAY_MS); walkToDifuUpperMerchant(); oilSalePrice1 = queryNpcSalePrice("地府货商", "油"); }
        BOT_LOG("TencentBot", "  地府货商 油收购价=" << oilSalePrice1);

        checkStop(stopSignal_);
        walkToDifuLowerMerchant();
        int oilSalePrice2 = queryNpcSalePrice("地府商人", "油");
        if (oilSalePrice2 < 0) { Sleep(Timing::UI_UPDATE_DELAY_MS); walkToDifuLowerMerchant(); oilSalePrice2 = queryNpcSalePrice("地府商人", "油"); }
        BOT_LOG("TencentBot", "  地府商人 油收购价=" << oilSalePrice2);

        // 2. 决策：谁卖价高给谁卖，然后标记另一个为"买入首选"（用于下一轮买纸钱）
        std::string sellTarget, nextBuyTarget;
        if (oilSalePrice1 >= oilSalePrice2) {
             sellTarget    = "地府货商"; // Upper
             nextBuyTarget = "地府商人"; // Lower
        } else {
             sellTarget    = "地府商人"; // Lower
             nextBuyTarget = "地府货商"; // Upper
        }

        BOT_LOG("TencentBot", "  决策: 卖给 " << sellTarget << "，下一轮首选买家更新为 " << nextBuyTarget);

        // 3. 执行卖出
        if (sellTarget == "地府货商") walkToDifuUpperMerchant();
        else walkToDifuLowerMerchant();
        
        int money = sellItemToNpc(sellTarget, "油");
        tryReturnToGangIfReached("difu_sale_oil", money);
        
        // 4. 更新首选（给 Step 1 用）
        ck.preferred_difu_merchant = nextBuyTarget;
        (void)store.save(ck);
    };

    // --- 操作表（名称 + 函数）---
    struct TradeStep {
        const char* name;
        std::function<void()> execute;
    };
    std::vector<TradeStep> steps = {
        {"leave_gang_to_difu",       step0_leaveBangpaiToDifu},
        {"difu_buy_paper",           step1_difuBuyPaper},
        {"travel_to_beiju",          step2_travelToBeiju},
        {"beiju_sell_paper_buy_oil", step3_beixuSellPaperBuyOil},
        {"return_to_difu",           step4_returnToDifu},
        {"difu_sell_oil",            step5_difuSellOil},
        // {"return_and_submit",        step6_returnAndSubmit}, // 不再顺序执行，仅由 tryReturnToGangIfReached 触发
    };

    // --- Checkpoint 版本迁移 ---
    if (ck.version < 2) {
        ck.next_op += 1;
        ck.version = 2;
        ck.last_op_name = "migrated_from_v1";
        (void)store.save(ck);
        BOT_LOG("TencentBot", "checkpoint v1→v2, next_op=" << ck.next_op);
    }
    if (ck.version < 3) {
        ck.version = 3;
        ck.last_op_name = "migrated_to_v3";
        (void)store.save(ck);
        BOT_LOG("TencentBot", "checkpoint 已迁移到 v3");
    }

    if (ck.next_op < 0) ck.next_op = 0;
    if (ck.next_op > static_cast<int>(steps.size())) ck.next_op = static_cast<int>(steps.size());

    // 后续循环跳过 step0（出帮派），从 step1 开始
    constexpr int kCycleRestartFromStep = 1;

    // --- 行为树版本主循环 ---
    // 节点设计（显式分发）：
    // 1) goal_branch: 目标达成后执行“回帮派并提交”
    // 2) step_dispatch_branch:
    //    - 每个 step 都是一个 Sequence(Condition(next_op==i), Action(run_step_i))
    //    - 所有 step Sequence 放入 Selector，实现“按 next_op 选择执行节点”
    // 3) cycle_restart_if_needed:
    //    - 当 next_op == steps.size() 时触发新一轮 cycle
    //
    // 根节点 route_root 仍使用 Selector，优先尝试 goal_branch，失败后走 step_dispatch_branch。
    auto btGoalCondition = [&]() -> bool {
        return ck.is_goal_reached;
    };

    // 当银票达标后，该动作负责执行最终提交流程。
    // step6_returnAndSubmit 内部在成功时会抛 GoalReachedException 结束主循环。
    auto btGoalAction = [&]() -> domain::TreeStatus {
        step6_returnAndSubmit();
        // 理论上若未抛异常，说明流程未真正收敛；短暂等待后让下一 tick 重试。
        BOT_WARN("TencentBot", "回帮步骤未完成，等待 5 秒后重试...");
        Sleep(5000);
        return domain::TreeStatus::Success;
    };

    // 封装单步执行（供每个 step Action 节点复用）。
    auto btRunStep = [&](int i) -> domain::TreeStatus {
        if (i < 0 || i >= static_cast<int>(steps.size())) {
            return domain::TreeStatus::Failure;
        }
        checkStop(stopSignal_);
        BOT_LOG("TencentBot", "=== step " << i << "/" << steps.size()
                << " : " << steps[i].name << " (cycle=" << ck.cycle << ") ===");
        steps[i].execute();

        if (!ck.is_goal_reached) {
            // 仅在未达标时推进断点；达标分支会由 goal_branch 接管。
            ck.next_op = i + 1;
            ck.last_op_name = steps[i].name;
            (void)store.save(ck);
        }
        return domain::TreeStatus::Success;
    };

    // 封装“轮次边界处理”：
    // 只有当 next_op 已走到末尾时才返回 Success，否则返回 Failure 让 selector 继续其它分支。
    auto btCycleRestartIfNeeded = [&]() -> domain::TreeStatus {
        if (ck.next_op < 0) ck.next_op = 0;
        if (ck.next_op > static_cast<int>(steps.size())) ck.next_op = static_cast<int>(steps.size());
        if (ck.next_op < static_cast<int>(steps.size())) {
            return domain::TreeStatus::Failure;
        }

        ck.cycle += 1;
        ck.next_op = kCycleRestartFromStep;
        ck.last_op_name = "cycle_restart";
        (void)store.save(ck);
        BOT_LOG("TencentBot", "未达标，继续下一轮 cycle=" << ck.cycle);
        return domain::TreeStatus::Success;
    };

    // 行为树节点状态观测：
    // 仅在状态变化时打日志，避免每 tick 刷屏。
    std::map<std::string, domain::TreeStatus> btLastStatus;
    auto btObserver = [&](std::string_view node, domain::TreeStatus status) {
        const std::string key(node);
        auto it = btLastStatus.find(key);
        if (it == btLastStatus.end() || it->second != status) {
            btLastStatus[key] = status;
            BOT_LOG("BehaviorTree", "node=" << key << " status=" << domain::status_to_cstr(status));
        }
    };

    // 将步骤执行器映射为 domain 层可消费的抽象节点定义。
    std::vector<domain::TradingStepNode> stepNodes;
    stepNodes.reserve(steps.size());
    for (int i = 0; i < static_cast<int>(steps.size()); ++i) {
        stepNodes.push_back(domain::TradingStepNode{
            std::string(steps[i].name),
            [&, i]() { return btRunStep(i); }
        });
    }

    domain::TradingTreeBuildContext treeCtx{};
    treeCtx.is_goal_reached = btGoalCondition;
    treeCtx.run_goal_action = btGoalAction;
    treeCtx.steps = std::move(stepNodes);
    treeCtx.current_step_index = [&]() { return ck.next_op; };
    treeCtx.cycle_restart_if_needed = btCycleRestartIfNeeded;
    treeCtx.observer = btObserver;

    domain::TradingRouteBehavior behavior(std::move(treeCtx));

    try {
        domain::RunTradingRouteTreeLoop(
            behavior,
            [&]() {
                // 非阻塞行为树模式下，step 动作在后台执行；
                // 这里仅做停止检查，避免与后台动作并发读写 checkpoint 状态。
                checkStop(stopSignal_);
            },
            [&]() {
                BOT_WARN("TencentBot", "行为树 tick 失败，1 秒后重试");
                Sleep(1000);
            }
        );
    } catch (const StopRequestedException&) {
        // 收到停止信号时，保证 checkpoint 至少落盘一次。
        (void)store.save(ck);
        BOT_LOG("TencentBot", "已请求停止，checkpoint 已保存 (next_op=" << ck.next_op << ")");
    } catch (const GoalReachedException&) {
        // 目标达成是正常业务收尾，不视为异常错误。
        BOT_LOG("TencentBot", "========== 跑商完成 ==========");
    }
}

void TencentBot::process_idiom_verify() {
    BOT_LOG("TencentBot", "开始成语验证流程...");

    // 1. 寻找总管
    auto npc_hits = vision.findNpcOnScreen("白虎堂总管");
    if (npc_hits.empty()) {
        BOT_ERR("TencentBot", "未找到「白虎堂总管」，请确认是否在金库门口");
        return;
    }
    Point2D pos_baihutangzongguan = npc_hits[0];

    // 2. 给予操作 (Alt+G)
    Send::KeyboardModifiers m = {0};
    m.LAlt = true;
    IbSendKeybdDownUp('G', m);
    Sleep(Timing::UI_CLICK_DELAY);

    // 点击总管弹出给予界面
    clickUiElement(pos_baihutangzongguan.x, pos_baihutangzongguan.y, 0, -50);
    moveCharacterTo(100, 100, 0); // 移走鼠标避免遮挡
    Sleep(Timing::UI_UPDATE_DELAY_MS);

    // 3. 选中银票
    auto v_yinpiao = vision.findNpcOnScreen("银票");
    if (v_yinpiao.empty()) {
        BOT_ERR("TencentBot", "给予界面中未找到「银票」");
        return;
    }
    clickUiElement(v_yinpiao[0].x, v_yinpiao[0].y);
    Sleep(Timing::UI_CLICK_DELAY);

    // 4. 点击确定给予
    auto v_confirm = vision.findNpcOnScreen("确定给与");
    if (v_confirm.empty()) {
        BOT_ERR("TencentBot", "未找到「确定给与」按钮");
        return;
    }
    clickUiElement(v_confirm[0].x, v_confirm[0].y);
    moveCharacterTo(100, 100, 0); 
    Sleep(Timing::UI_UPDATE_DELAY_MS * 3); // 等待验证码弹出

    // 5-7. 成语识别 + 点击 + 确认（支持答错重试）
    constexpr int kMaxCaptchaRetries = 3;
    for (int attempt = 1; attempt <= kMaxCaptchaRetries; ++attempt) {
        checkStop(stopSignal_);
        BOT_LOG("TencentBot", "成语验证尝试 " << attempt << "/" << kMaxCaptchaRetries);

        // 5. 识别成语
        vision.captureToBuffer();
        if (vision.imageBuffer.empty()) return;

        std::string idiom = aiCaptcha->recognizeIdiom(vision.frameW, vision.frameH, vision.imageBuffer);
        if (idiom.empty()) {
            BOT_ERR("TencentBot", "无法识别成语");
            return;
        }
        BOT_LOG("TencentBot", "目标成语: " << idiom);

        // 将 UTF-8 字符串拆分为单字列表
        std::vector<std::string> targetChars;
        for (size_t i = 0; i < idiom.length(); ) {
            unsigned char c = static_cast<unsigned char>(idiom[i]);
            int len = 1;
            if (c >= 0xf0) len = 4;
            else if (c >= 0xe0) len = 3;
            else if (c >= 0xc0) len = 2;
            targetChars.push_back(idiom.substr(i, len));
            i += len;
        }

        // 6. 按序点击汉字
        bool clickFailed = false;
        for (size_t i = 0; i < targetChars.size(); ++i) {
            checkStop(stopSignal_);
            std::string charToClick = targetChars[i];

            // 统计该字在成语中是第几次出现 (用于处理叠词，比如"抽抽噎噎")
            int occurrence = 0;
            for (size_t j = 0; j < i; ++j) {
                if (targetChars[j] == charToClick) occurrence++;
            }

            // 每次点击都要重新截图，因为界面会刷新
            vision.captureToBuffer();
            auto charPositions = aiCaptcha->findChar(vision.frameW, vision.frameH, vision.imageBuffer, charToClick);

            if (charPositions.size() <= static_cast<size_t>(occurrence)) {
                BOT_ERR("TencentBot", "未找到汉字「" << charToClick << "」的第 " << (occurrence + 1) << " 个坐标");
                clickFailed = true;
                break;
            }
            Point2D targetPos = charPositions[occurrence];
            BOT_LOG("TencentBot", "点击 [ " << charToClick << " ] (" << (i + 1) << "/4)");
            clickUiElement(targetPos.x, targetPos.y);
            moveCharacterTo(100, 100, 0);
            Sleep(Timing::UI_UPDATE_DELAY_MS);
        }

        // 7. 点击确认按钮
        if (!clickFailed) {
            auto v_chengyuqueren = vision.findNpcOnScreen("成语确认");
            if (!v_chengyuqueren.empty()) {
                clickUiElement(v_chengyuqueren[0].x, v_chengyuqueren[0].y);
                moveCharacterTo(100, 100, 0);
                Sleep(Timing::UI_UPDATE_DELAY_MS * 2); // 等待服务器判定结果
            } else {
                BOT_WARN("TencentBot", "未找到「成语确认」按钮");
            }
        }

        // 8. 检查是否答对：如果"重置"按钮不存在，说明答对了
        auto v_reset = vision.findNpcOnScreen("重置");
        if (v_reset.empty()) {
            BOT_LOG("TencentBot", "成语验证通过！");
            break;
        }
        // 答错了，点击重置并重试
        BOT_WARN("TencentBot", "成语答案错误，点击重置 (尝试 " << attempt << "/" << kMaxCaptchaRetries << ")");
        clickUiElement(v_reset[0].x, v_reset[0].y);
        moveCharacterTo(100, 100, 0);
        Sleep(Timing::UI_UPDATE_DELAY_MS * 2); // 等待界面刷新后重新出题
    }
    BOT_LOG("TencentBot", "成语验证流程结束");
}
