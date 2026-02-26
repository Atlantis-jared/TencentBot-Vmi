#include "TencentBot.h"

#include "config/BotSettings.h"
#include "domain/RunControl.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "../IbInputSimulator/Simulator/include/IbInputSimulator/InputSimulator.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <string>

namespace {

const config::BotSettings& settings() {
    return config::GetBotSettings();
}

const config::TimingSettings& timing() {
    return settings().timing;
}

namespace MouseCurve {
constexpr double MOVE_SCALE = 0.35;
constexpr double OVERSHOOT_MIN = 1.5;
constexpr double OVERSHOOT_MAX = 3.0;
constexpr double PULLBACK_FACTOR = 0.25;
constexpr double ARRIVAL_THRESHOLD = 2.0;
constexpr int PULLBACK_MAX_ITERS = 15;
constexpr double SHORT_MOVE_THRESHOLD = 6.0;
} // namespace MouseCurve

namespace WalkConfig {
constexpr int TIMEOUT_MS = 25000;
constexpr int POLL_INTERVAL_MS = 500;
constexpr int ARRIVAL_RADIUS = 4;
} // namespace WalkConfig

} // namespace

void TencentBot::moveCharacterTo(int targetX, int targetY, int processIndex) {
    using namespace MouseCurve;

    RawCoord currentRaw = gameMemory.readPitPosRaw(static_cast<uint32_t>(processIndex));
    double startX = static_cast<double>(currentRaw.x);
    double startY = static_cast<double>(currentRaw.y);

    double deltaX = targetX - startX;
    double deltaY = targetY - startY;
    double totalDist = std::hypot(deltaX, deltaY);
    if (totalDist < 1.0) {
        return;
    }

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

    double overshootX;
    double overshootY;
    double ctrl1X;
    double ctrl1Y;
    double ctrl2X;
    double ctrl2Y;
    int curveSteps;

    if (totalDist < SHORT_MOVE_THRESHOLD) {
        overshootX = targetX;
        overshootY = targetY;
        ctrl1X = startX + deltaX * 0.33;
        ctrl1Y = startY + deltaY * 0.33;
        ctrl2X = startX + deltaX * 0.66;
        ctrl2Y = startY + deltaY * 0.66;
        curveSteps = std::max(4, static_cast<int>(totalDist));
    } else {
        double angle = std::atan2(deltaY, deltaX);
        double overshootDist = randomReal(OVERSHOOT_MIN, OVERSHOOT_MAX);
        overshootX = targetX + std::cos(angle) * overshootDist;
        overshootY = targetY + std::sin(angle) * overshootDist;

        double sideSign = (randomInt(0, 1) == 0) ? 1.0 : -1.0;
        double offsetScale = totalDist * randomReal(0.05, 0.1);
        double normalX = -deltaY / totalDist;
        double normalY = deltaX / totalDist;

        ctrl1X = startX + (overshootX - startX) * 0.3 + normalX * offsetScale * sideSign;
        ctrl1Y = startY + (overshootY - startY) * 0.3 + normalY * offsetScale * sideSign;
        ctrl2X = startX + (overshootX - startX) * 0.7 + normalX * offsetScale * sideSign;
        ctrl2Y = startY + (overshootY - startY) * 0.7 + normalY * offsetScale * sideSign;
        curveSteps = std::clamp(static_cast<int>(totalDist / 3.5), 12, 50);
    }

    double errorAccumX = 0.0;
    double errorAccumY = 0.0;
    for (int step = 1; step <= curveSteps; ++step) {
        RawCoord nowRaw = gameMemory.readPitPosRaw(static_cast<uint32_t>(processIndex));
        double curX = static_cast<double>(nowRaw.x);
        double curY = static_cast<double>(nowRaw.y);
        if (std::hypot(overshootX - curX, overshootY - curY) < 1.0) {
            break;
        }

        double t = static_cast<double>(step) / curveSteps;
        double eased = t * (2.0 - t);
        double inv = 1.0 - eased;

        double bezX = inv * inv * inv * startX + 3 * inv * inv * eased * ctrl1X
            + 3 * inv * eased * eased * ctrl2X + eased * eased * eased * overshootX;
        double bezY = inv * inv * inv * startY + 3 * inv * inv * eased * ctrl1Y
            + 3 * inv * eased * eased * ctrl2Y + eased * eased * eased * overshootY;

        double moveX = (bezX - curX) * MOVE_SCALE + errorAccumX;
        double moveY = (bezY - curY) * MOVE_SCALE + errorAccumY;
        int intMoveX = static_cast<int>(std::round(moveX));
        int intMoveY = static_cast<int>(std::round(moveY));
        errorAccumX = moveX - intMoveX;
        errorAccumY = moveY - intMoveY;

        if (intMoveX != 0 || intMoveY != 0) {
            IbSendMouseMove(intMoveX, intMoveY, Send::MoveMode::Relative);
        }
        domain::sleep_interruptible(stopSignal_, 1 + randomInt(0, 1));
    }

    domain::sleep_interruptible(stopSignal_, 15 + randomInt(0, 14));
    double prevDistToTarget = 1e9;
    int noImprovementCount = 0;

    for (int iter = 0; iter < PULLBACK_MAX_ITERS; ++iter) {
        RawCoord finalRaw = gameMemory.readPitPosRaw(static_cast<uint32_t>(processIndex));
        double curX = static_cast<double>(finalRaw.x);
        double curY = static_cast<double>(finalRaw.y);
        double diffX = targetX - curX;
        double diffY = targetY - curY;
        double distToTarget = std::hypot(diffX, diffY);

        if (distToTarget <= ARRIVAL_THRESHOLD) {
            break;
        }
        if (distToTarget >= prevDistToTarget) {
            if (++noImprovementCount >= 2) {
                break;
            }
        } else {
            noImprovementCount = 0;
        }
        prevDistToTarget = distToTarget;

        int corrX = static_cast<int>(std::round(diffX * MOVE_SCALE * PULLBACK_FACTOR));
        int corrY = static_cast<int>(std::round(diffY * MOVE_SCALE * PULLBACK_FACTOR));
        if (corrX == 0 && std::abs(diffX) >= 1.0) {
            corrX = (diffX > 0 ? 1 : -1);
        }
        if (corrY == 0 && std::abs(diffY) >= 1.0) {
            corrY = (diffY > 0 ? 1 : -1);
        }

        IbSendMouseMove(corrX, corrY, Send::MoveMode::Relative);
        domain::sleep_interruptible(stopSignal_, 10 + randomInt(0, 4));
    }
}

void TencentBot::moveCharacterToOffset(int targetX, int targetY, int processIndex,
    int extraOffsetX, int extraOffsetY) {
    moveCharacterTo(targetX + extraOffsetX, targetY + extraOffsetY, processIndex);
}

void TencentBot::clickMapPosition(const std::string& mapName, int gameX, int gameY, int processIndex) {
    const auto& mapProps = settings().map_properties;
    auto propIter = mapProps.find(mapName);
    if (propIter == mapProps.end()) {
        BOT_WARN("TencentBot", "clickMapPosition 未找到地图属性: " << mapName);
        return;
    }
    (void)processIndex;
    const MapProperties& mp = propIter->second;

    constexpr int MAP_UI_RETRY_WAIT_MS = 100;
    constexpr int MAP_UI_MAX_ATTEMPTS = 5;

    MapUiLocation uiLoc = vision.locateMapUiOnScreen(mapName);
    for (int attempt = 1; !uiLoc.found && attempt < MAP_UI_MAX_ATTEMPTS; ++attempt) {
        BOT_WARN("TencentBot", "小地图 UI 定位失败: " << mapName << "，重试 " << attempt << "/" << (MAP_UI_MAX_ATTEMPTS - 1));
        domain::sleep_interruptible(stopSignal_, MAP_UI_RETRY_WAIT_MS);
        uiLoc = vision.locateMapUiOnScreen(mapName);
    }
    if (!uiLoc.found) {
        BOT_ERR("TencentBot", "小地图 UI 最终未找到: " << mapName << "，跳过本次点击");
        return;
    }

    double originX = static_cast<double>(uiLoc.topLeftCorner.x);
    double originY = static_cast<double>(uiLoc.topLeftCorner.y + uiLoc.uiHeight);

    double pixelX = originX + (static_cast<double>(gameX) * mp.uiPixelWidth / mp.gameCoordMaxX);
    double pixelY = originY - (static_cast<double>(gameY) * mp.uiPixelHeight / mp.gameCoordMaxY);

    clickUiElement(static_cast<int>(std::round(pixelX)),
        static_cast<int>(std::round(pixelY)));
}

void TencentBot::clickUiElement(int screenX, int screenY, int extraOffX, int extraOffY) {
    const auto& offset = settings().window_offset;
    moveCharacterToOffset(screenX, screenY, 0,
        offset.x + extraOffX,
        offset.y + extraOffY);
    IbSendMouseClick(Send::MouseButton::Left);
    domain::sleep_interruptible(stopSignal_, timing().ui_update_delay_ms);
}

void TencentBot::hideOtherPlayers() {
    Send::KeyboardModifiers altKey{ 0 };
    altKey.LAlt = true;
    IbSendKeybdDownUp('H', altKey);
    domain::sleep_interruptible(stopSignal_, timing().key_action_delay_ms);
    IbSendKeybdDown(VK_F9);
    IbSendKeybdUp(VK_F9);
    domain::sleep_interruptible(stopSignal_, timing().key_action_delay_ms);
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

void TencentBot::walkToPosition(const std::string& mapName, int targetX, int targetY) {
    BOT_LOG("TencentBot", "寻路: " << mapName << " → (" << targetX << "," << targetY << ")");
    domain::check_stop_or_throw(stopSignal_);

    IbSendKeybdDown(VK_TAB);
    IbSendKeybdUp(VK_TAB);
    domain::sleep_interruptible(stopSignal_, timing().key_action_delay_ms);

    clickMapPosition(mapName, targetX, targetY, 0);
    IbSendMouseClick(Send::MouseButton::Left);
    domain::sleep_interruptible(stopSignal_, timing().ui_update_delay_ms);

    IbSendKeybdDown(VK_TAB);
    IbSendKeybdUp(VK_TAB);
    domain::sleep_interruptible(stopSignal_, timing().key_action_delay_ms);

    int elapsedMs = 0;
    int stagnantIters = 0;
    double minDistance = 999999.0;
    int lastClickElapsedMs = 0;

    while (elapsedMs < WalkConfig::TIMEOUT_MS) {
        domain::check_stop_or_throw(stopSignal_);
        std::string curMap = vision.getCurrentMapName();
        GameCoord curPos = gameMemory.readRoleGameCoord(0, curMap);

        double dist = std::hypot(curPos.x - targetX, curPos.y - targetY);
        if (dist < WalkConfig::ARRIVAL_RADIUS) {
            break;
        }

        if (dist < minDistance - 0.5) {
            minDistance = dist;
            stagnantIters = 0;
        } else {
            stagnantIters++;
        }

        if (stagnantIters >= 6 || (elapsedMs - lastClickElapsedMs >= 8000)) {
            BOT_LOG("TencentBot", "检测到寻路停滞/超时，重新点击小地图 (" << stagnantIters << " iters)");

            IbSendKeybdDown(VK_TAB);
            IbSendKeybdUp(VK_TAB);
            domain::sleep_interruptible(stopSignal_, timing().key_action_delay_ms);
            clickMapPosition(mapName, targetX, targetY, 0);
            IbSendMouseClick(Send::MouseButton::Left);
            domain::sleep_interruptible(stopSignal_, timing().ui_update_delay_ms);
            IbSendKeybdDown(VK_TAB);
            IbSendKeybdUp(VK_TAB);
            domain::sleep_interruptible(stopSignal_, timing().key_action_delay_ms);

            stagnantIters = 0;
            lastClickElapsedMs = elapsedMs;
        }

        domain::sleep_interruptible(stopSignal_, WalkConfig::POLL_INTERVAL_MS);
        elapsedMs += WalkConfig::POLL_INTERVAL_MS;
    }

    if (elapsedMs >= WalkConfig::TIMEOUT_MS) {
        BOT_WARN("TencentBot", "寻路最终超时: " << mapName << " (" << targetX << "," << targetY << ")");
    }
    domain::sleep_interruptible(stopSignal_, timing().ui_update_delay_ms);
}

bool TencentBot::tryNpcTeleport(const std::string& npcName,
    int npcClickOffX, int npcClickOffY,
    int menuClickX, int menuClickY,
    const std::string& expectedMap,
    const std::string& retryWalkMap,
    int retryWalkX, int retryWalkY, int maxRetries) {
    BOT_LOG("TencentBot", "传送(NPC): " << npcName << " → " << expectedMap);
    for (int attempt = 0; attempt < maxRetries; ++attempt) {
        domain::check_stop_or_throw(stopSignal_);
        BOT_LOG("TencentBot", "  尝试 " << (attempt + 1) << "/" << maxRetries);
        if (attempt > 0) {
            hideOtherPlayers();
            domain::sleep_interruptible(stopSignal_, timing().key_action_delay_ms);
            walkToPosition(retryWalkMap, retryWalkX, retryWalkY);
            domain::sleep_interruptible(stopSignal_, timing().ui_update_delay_ms);
        }
        if (clickNpcIfFound(npcName, npcClickOffX, npcClickOffY)) {
            domain::check_stop_or_throw(stopSignal_);
            clickUiElement(menuClickX, menuClickY);
            domain::sleep_interruptible(stopSignal_, timing().map_change_delay_ms);
            std::string currentMap = vision.getCurrentMapName();
            if (currentMap == expectedMap) {
                BOT_LOG("TencentBot", "  到达 " << expectedMap);
                domain::sleep_interruptible(stopSignal_, timing().key_action_delay_ms);
                return true;
            }
            if (currentMap == "CaptureEmpty" && screenCapture.recreateIfNeeded()) {
                domain::sleep_interruptible(stopSignal_, timing().ui_update_delay_ms);
            }
        }
        domain::sleep_interruptible(stopSignal_, 300);
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
        domain::check_stop_or_throw(stopSignal_);
        BOT_LOG("TencentBot", "  尝试 " << (attempt + 1) << "/" << maxRetries);
        if (attempt > 0) {
            hideOtherPlayers();
            domain::sleep_interruptible(stopSignal_, timing().key_action_delay_ms);
            walkToPosition(retryWalkMap, retryWalkX, retryWalkY);
            domain::sleep_interruptible(stopSignal_, timing().ui_update_delay_ms);
        }
        clickUiElement(clickX, clickY, extraOffX, extraOffY);
        domain::sleep_interruptible(stopSignal_, timing().map_change_delay_ms);
        std::string currentMap = vision.getCurrentMapName();
        if (currentMap == expectedMap) {
            BOT_LOG("TencentBot", "  到达 " << expectedMap);
            domain::sleep_interruptible(stopSignal_, timing().key_action_delay_ms);
            return true;
        }
        if (currentMap == "CaptureEmpty" && screenCapture.recreateIfNeeded()) {
            domain::sleep_interruptible(stopSignal_, timing().ui_update_delay_ms);
        }
        domain::sleep_interruptible(stopSignal_, 300);
    }
    BOT_ERR("TencentBot", "传送失败: " << expectedMap << " " << maxRetries << " 次后未到达");
    return false;
}
