#pragma once
// =============================================================================
// GameMemory.h — 游戏进程内存读取层
// =============================================================================
// 本模块封装基于 SharedDataStatus 的坐标读取和换算逻辑。
// 当前数据来源为 Host 侧 Rust memflow 后端写入，不再直接读目标进程虚拟内存。
//
// 依赖：
//   - MemoryReader.h（vsock INIT_BIND 握手）
//   - BotLogger.h（统一日志）
//
// 地图坐标系说明：
//   游戏内坐标（rawX, rawY）是从内存读出的原始整数。
//   换算公式（以 changancheng 为例）：
//     gameX = (rawX - 10) / 20
//     gameY = (Y_BASE_MAP["changancheng"] - rawY) / 20
//   不同地图的 Y_BASE 不同，存储在内部 kMapYBase 表中。
// =============================================================================

#include <cstdint>
#include <string>
#include <utility> // std::pair
#include <vector>
#include <map>
#include <memory>

#include "MemoryReader.h"

// -----------------------------------------------------------------------------
// 内存偏移量（握手协议字段）：
// 由 Guest 提供给 Host Rust memflow 后端用于解析游戏坐标链。
// -----------------------------------------------------------------------------
#define GAME_PIT_CHAIN_BASE_OFFSET   0x023d0210  // mhmain.dll + 此偏移 = 光标的的一级指针
#define GAME_PIT_POS_STRUCT_OFFSET   0x118       // 一级指针 + 此偏移 = 坐标结构体首地址（X 在 +0，Y 在 +4）
#define GAME_ROLE_CHAIN_BASE_OFFSET  0x023d0210  // 同一基址（pit 与 role 共享入口）
#define GAME_ROLE_POS_STRUCT_OFFSET  0x208       // 游戏逻辑坐标结构体偏移

// -----------------------------------------------------------------------------
// RawCoord — 从内存读出的原始坐标（整数像素单位，未换算）
// -----------------------------------------------------------------------------
struct RawCoord {
    uint32_t x; // 水平原始值
    uint32_t y; // 垂直原始值
};

// -----------------------------------------------------------------------------
// GameCoord — 换算后的游戏世界坐标（寻路和比较均使用此坐标）
// -----------------------------------------------------------------------------
struct GameCoord {
    int x; // 游戏世界 X（向右为正）
    int y; // 游戏世界 Y（向下为正）
};

// =============================================================================
// GameMemory — 内存访问管理类
// =============================================================================
class GameMemory {
public:
    // -------------------------------------------------------------------------
    // 进程描述符（由 TencentBot::init() 填充后注入）
    // 每个元素对应一个 mhmain.exe 进程实例（多开场景）。
    // -------------------------------------------------------------------------
    std::vector<uint64_t> processIds;    // 各游戏进程的 PID
    std::vector<uint64_t> dllBaseAddrs;  // 各进程 mhmain.dll 的虚拟基地址

    GameMemory() = default;

    void set_memory_reader(const std::shared_ptr<IProcessMemoryReader>& reader);
    bool initialize_module_bases(const std::string& module_name, std::string* error);

    // -------------------------------------------------------------------------
    // readPitPosRaw() — 读取指定进程的原始"坑位"坐标
    // 参数：
    //   processIndex — 多开时的进程编号（0 = 第一个主进程）
    // 返回：
    //   包含 (rawX, rawY) 的 RawCoord 结构，整数像素单位
    // 注意：
    //   此函数执行三次后端读操作（读一级指针 + 读 X + 读 Y）。
    // -------------------------------------------------------------------------
    RawCoord readPitPosRaw(uint32_t processIndex) const;

    // -------------------------------------------------------------------------
    // readRoleGameCoord() — 读取角色游戏世界坐标（已换算）
    // 参数：
    //   processIndex — 多开时的进程编号
    //   currentMapName — 当前地图名（用于查找 Y_BASE，需外部传入）
    // 返回：
    //   换算后的 GameCoord；若地图名未知则 y = 0 并打 WARN 日志
    // -------------------------------------------------------------------------
    GameCoord readRoleGameCoord(uint32_t processIndex, const std::string& currentMapName) const;

private:
    std::shared_ptr<IProcessMemoryReader> reader_;

    // -------------------------------------------------------------------------
    // kMapYBase — 各地图的 Y 坐标基准值（游戏地图左上角对应的 rawY）
    // 公式：gameY = (kMapYBase[mapName] - rawY) / 20
    // 如需新增地图，直接在 GameMemory.cpp 中扩充此表。
    // -------------------------------------------------------------------------
    static const std::map<std::string, int> kMapYBase;
};
