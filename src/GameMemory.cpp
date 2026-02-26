// =============================================================================
// GameMemory.cpp — 游戏进程内存读取实现
// =============================================================================
#include "GameMemory.h"
#include "BotLogger.h"
#include "SharedDataStatus.h"

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
    {"bangpai",         4150},   // 帮派（能正确获取坐标但是没有小地图进行寻路
    {"chishuizhou",     1910},   // 赤水洲
    {"nvbamu",          2870},   // 女魃墓
    {"donghaiyandong",  1790},   // 东海岩洞
    {"huaguoshan",      2390},   // 花果山
    {"jinku",           1470},   // 帮派金库 能正确获取坐标但是没有小地图进行寻路
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
    if (processIndex >= processIds.size()) {
        BOT_ERR("GameMemory", "readPitPosRaw 参数越界或 reader 未初始化");
        return {};
    }
    if (reader_ && reader_->uses_shared_data_feed()) {
        const SharedDataSnapshot snap = read_shared_data_snapshot();
        if (snap.sync_flag != 1) {
            BOT_WARN("GameMemory", "SharedDataStatus 未就绪，sync_flag=" << snap.sync_flag);
            return {};
        }
        if (snap.current_x < 0 || snap.current_y < 0) {
            BOT_WARN("GameMemory", "SharedDataStatus 坐标异常: x=" << snap.current_x << " y=" << snap.current_y);
            return {};
        }
        return RawCoord{
            static_cast<std::uint32_t>(snap.current_x),
            static_cast<std::uint32_t>(snap.current_y),
        };
    }
    if (!reader_ || processIndex >= dllBaseAddrs.size()) {
        BOT_ERR("GameMemory", "readPitPosRaw 参数越界或 reader 未初始化");
        return {};
    }

    // 第一次读：从 mhmain.dll + 基址偏移处，取出一级指针（8 字节）
    uint64_t firstLevelPtr = 0;
    std::string err;
    if (!reader_->read_virtual_by_pid(
            processIds[processIndex],
            dllBaseAddrs[processIndex] + GAME_PIT_CHAIN_BASE_OFFSET,
            &firstLevelPtr,
            sizeof(firstLevelPtr),
            &err)) {
        BOT_ERR("GameMemory", "读取一级指针失败: " << err);
        return {};
    }

    // 第二次读：从一级指针 + 结构体偏移处，读取 X 和 Y（各 4 字节）
    RawCoord rawCoord{};
    if (!reader_->read_virtual_by_pid(
            processIds[processIndex],
            firstLevelPtr + GAME_PIT_POS_STRUCT_OFFSET,
            &rawCoord.x,
            sizeof(rawCoord.x),
            &err)) {
        BOT_ERR("GameMemory", "读取 rawX 失败: " << err);
        return {};
    }
    if (!reader_->read_virtual_by_pid(
            processIds[processIndex],
            firstLevelPtr + GAME_PIT_POS_STRUCT_OFFSET + sizeof(uint32_t),
            &rawCoord.y,
            sizeof(rawCoord.y),
            &err)) {
        BOT_ERR("GameMemory", "读取 rawY 失败: " << err);
        return {};
    }

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
    if (processIndex >= processIds.size()) {
        BOT_ERR("GameMemory", "readRoleGameCoord 参数越界或 reader 未初始化");
        return {};
    }
    if (reader_ && reader_->uses_shared_data_feed()) {
        const SharedDataSnapshot snap = read_shared_data_snapshot();
        if (snap.sync_flag != 1) {
            BOT_WARN("GameMemory", "SharedDataStatus 未就绪，sync_flag=" << snap.sync_flag);
            return {};
        }

        GameCoord result{};
        result.x = (snap.current_x - 10) / 20;
        auto yBaseIter = kMapYBase.find(currentMapName);
        if (yBaseIter != kMapYBase.end()) {
            result.y = (yBaseIter->second - snap.current_y) / 20;
        } else {
            BOT_WARN("GameMemory", "未知地图名「" << currentMapName << "」，无法换算 Y 坐标，返回 0");
            result.y = 0;
        }
        return result;
    }
    if (!reader_ || processIndex >= dllBaseAddrs.size()) {
        BOT_ERR("GameMemory", "readRoleGameCoord 参数越界或 reader 未初始化");
        return {};
    }

    // --- 第一次读：取一级角色指针（与 pit 相同基址，但二级偏移不同）---
    uint64_t firstLevelPtr = 0;
    std::string err;
    if (!reader_->read_virtual_by_pid(
            processIds[processIndex],
            dllBaseAddrs[processIndex] + GAME_ROLE_CHAIN_BASE_OFFSET,
            &firstLevelPtr,
            sizeof(firstLevelPtr),
            &err)) {
        BOT_ERR("GameMemory", "读取角色一级指针失败: " << err);
        return {};
    }

    // --- 第二次读：取角色游戏逻辑坐标（与 pit 不同的二级偏移）---
    uint32_t rawX = 0, rawY = 0;
    if (!reader_->read_virtual_by_pid(
            processIds[processIndex],
            firstLevelPtr + GAME_ROLE_POS_STRUCT_OFFSET,
            &rawX,
            sizeof(rawX),
            &err)) {
        BOT_ERR("GameMemory", "读取角色 rawX 失败: " << err);
        return {};
    }
    if (!reader_->read_virtual_by_pid(
            processIds[processIndex],
            firstLevelPtr + GAME_ROLE_POS_STRUCT_OFFSET + sizeof(uint32_t),
            &rawY,
            sizeof(rawY),
            &err)) {
        BOT_ERR("GameMemory", "读取角色 rawY 失败: " << err);
        return {};
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
void GameMemory::set_memory_reader(const std::shared_ptr<IProcessMemoryReader>& reader) {
    reader_ = reader;
}

bool GameMemory::initialize_module_bases(const std::string& module_name, std::string* error) {
    if (!reader_) {
        if (error) {
            *error = "memory reader is not set";
        }
        return false;
    }

    if (!processIds.empty()) {
        std::string bindErr;
        if (!reader_->initialize_binding(processIds.front(), &bindErr)) {
            if (error) {
                *error = "initialize_binding failed: " + bindErr;
            }
            return false;
        }
    }

    if (reader_->uses_shared_data_feed()) {
        dllBaseAddrs.assign(processIds.size(), 0);
        BOT_LOG("GameMemory", "使用 SharedDataStatus 数据面，跳过模块基址查询");
        return true;
    }

    dllBaseAddrs.clear();
    dllBaseAddrs.reserve(processIds.size());

    for (std::size_t i = 0; i < processIds.size(); ++i) {
        std::uint64_t base = 0;
        std::string local_err;
        if (!reader_->query_module_base_by_pid(processIds[i], module_name, &base, &local_err)) {
            if (error) {
                *error = "query_module_base_by_pid failed for pid=" + std::to_string(processIds[i]) +
                         ": " + local_err;
            }
            return false;
        }
        dllBaseAddrs.push_back(base);
        BOT_LOG("GameMemory", "进程[" << i << "] PID=" << processIds[i]
                << " DllBase=0x" << std::hex << base << std::dec);
    }
    return true;
}
