// =============================================================================
// GameMemory.cpp — 游戏进程内存读取实现
// =============================================================================
#include "GameMemory.h"
#include "BotLogger.h"
#include "MemflowConnector.h"
#include "../hv.h"

// -----------------------------------------------------------------------------
// kMapYBase — 各地图 Y 坐标基准值（rawY → gameY 的换算锚点）
// 换算公式：gameY = (kMapYBase[mapName] - rawY) / 20
// 新地图只需在此处追加一行，上层代码无需改动。
// -----------------------------------------------------------------------------
const std::map<std::string, int> GameMemory::kMapYBase = {
    {"changancheng",    5590},   // 长安城
    {"difu",            2390},   // 地府
    {"datangguojing",   6710},   // 大唐国境
    {"beijuluzhou",     3390},   // 北俱泸州
    {"aolaiguo",        3010},   // 傲来国（花果山附近）
    {"donghaiwan",      2390},   // 东海湾
    {"bangpai",         4150},   // 帮派（内部地图）
    {"chishuizhou",     1910},   // 赤水洲
    {"nvbamu",          2870},   // 女魃墓
    {"donghaiyandong",  1790},   // 东海岩洞
    {"huaguoshan",      2390},   // 花果山
    {"jinku",           1470},   // 帮派金库（需提供 maps/jinku.png）
};

// -----------------------------------------------------------------------------
// readPitPosRaw() — 两级指针解引用，从目标进程虚拟内存读取原始像素坐标
//
// 指针链（以进程 index=0 为例）：
//   dllBaseAddrs[0] + GAME_PIT_CHAIN_BASE_OFFSET
//       → 一级指针 (uint64_t)
//           + GAME_PIT_POS_STRUCT_OFFSET
//               → rawX (uint32_t) at +0
//               → rawY (uint32_t) at +4
// -----------------------------------------------------------------------------
RawCoord GameMemory::readPitPosRaw(uint32_t processIndex) const {
    if (memflowConn && memflowConn->isActive()) {
        const auto* buf = memflowConn->getBuffer();
        return { (uint32_t)buf->pit_x, (uint32_t)buf->pit_y };
    }

    // 第一次读：从 mhmain.dll + 基址偏移处，取出一级指针（8 字节）
    uint64_t firstLevelPtr = 0;
    hv::read_virt_mem(
        cr3Values[processIndex],               // 目标进程的 CR3（页表基址）
        &firstLevelPtr,                        // 写入本地变量
        reinterpret_cast<void*>(
            dllBaseAddrs[processIndex] + GAME_PIT_CHAIN_BASE_OFFSET
        ),
        sizeof(firstLevelPtr)
    );

    // 第二次读：从一级指针 + 结构体偏移处，读取 X 和 Y（各 4 字节）
    RawCoord rawCoord{};
    hv::read_virt_mem(
        cr3Values[processIndex],
        &rawCoord.x,
        reinterpret_cast<void*>(firstLevelPtr + GAME_PIT_POS_STRUCT_OFFSET),
        sizeof(rawCoord.x)
    );
    hv::read_virt_mem(
        cr3Values[processIndex],
        &rawCoord.y,
        reinterpret_cast<void*>(firstLevelPtr + GAME_PIT_POS_STRUCT_OFFSET + sizeof(uint32_t)),
        sizeof(rawCoord.y)
    );

    return rawCoord;
}

// -----------------------------------------------------------------------------
// readRoleGameCoord() — 读取角色游戏世界坐标并换算
//
// 换算规则：
//   gameX = (rawX - 10) / 20
//   gameY = (kMapYBase[currentMapName] - rawY) / 20
//
// 若 currentMapName 不在 kMapYBase 中，gameY 返回 0，并输出 WARN 日志。
// 调用方应先通过 VisionEngine::getCurrentMapName() 获取正确的地图名再传入。
// -----------------------------------------------------------------------------
GameCoord GameMemory::readRoleGameCoord(uint32_t processIndex, const std::string& currentMapName) const {
    uint32_t rawX = 0, rawY = 0;

    if (memflowConn && memflowConn->isActive()) {
        const auto* buf = memflowConn->getBuffer();
        rawX = (uint32_t)buf->role_raw_x;
        rawY = (uint32_t)buf->role_raw_y;
    } else {
        // --- 第一次读：取一级角色指针（与 pit 相同基址，但二级偏移不同）---
        uint64_t firstLevelPtr = 0;
        hv::read_virt_mem(
            cr3Values[processIndex],
            &firstLevelPtr,
            reinterpret_cast<void*>(
                dllBaseAddrs[processIndex] + GAME_ROLE_CHAIN_BASE_OFFSET
            ),
            sizeof(firstLevelPtr)
        );

        // --- 第二次读：取角色游戏逻辑坐标（与 pit 不同的二级偏移）---
        hv::read_virt_mem(
            cr3Values[processIndex],
            &rawX,
            reinterpret_cast<void*>(firstLevelPtr + GAME_ROLE_POS_STRUCT_OFFSET),
            sizeof(rawX)
        );
        hv::read_virt_mem(
            cr3Values[processIndex],
            &rawY,
            reinterpret_cast<void*>(firstLevelPtr + GAME_ROLE_POS_STRUCT_OFFSET + sizeof(uint32_t)),
            sizeof(rawY)
        );
    }

    // --- 坐标换算 ---
    GameCoord result{};
    result.x = (static_cast<int>(rawX) - 10) / 20; // X 轴统一换算

    auto yBaseIter = kMapYBase.find(currentMapName);
    if (yBaseIter != kMapYBase.end()) {
        result.y = (yBaseIter->second - static_cast<int>(rawY)) / 20;
    } else {
        BOT_WARN("GameMemory", "未知地图名「" << currentMapName << "」，无法换算 Y 坐标，返回 0");
        result.y = 0;
    }

    return result;
}
