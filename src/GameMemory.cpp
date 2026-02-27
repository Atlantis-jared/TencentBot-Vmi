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
        BOT_ERR("GameMemory", "readPitPosRaw 参数越界");
        return {};
    }
    if (!reader_ || !reader_->uses_shared_data_feed()) {
        BOT_ERR("GameMemory", "仅支持 SharedDataStatus 数据面（Rust memflow 后端）");
        return {};
    }

    const SharedDataSnapshot snap = read_shared_data_snapshot();
    if (snap.sync_flag != 1) {
        BOT_WARN("GameMemory", "SharedDataStatus 未就绪，sync_flag=" << snap.sync_flag);
        return {};
    }
    if (snap.pit_x < 0 || snap.pit_y < 0) {
        BOT_WARN("GameMemory", "SharedDataStatus 坑位坐标异常: x=" << snap.pit_x << " y=" << snap.pit_y);
        return {};
    }

    return RawCoord{
        static_cast<std::uint32_t>(snap.pit_x),
        static_cast<std::uint32_t>(snap.pit_y),
    };
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
        BOT_ERR("GameMemory", "readRoleGameCoord 参数越界");
        return {};
    }
    if (!reader_ || !reader_->uses_shared_data_feed()) {
        BOT_ERR("GameMemory", "仅支持 SharedDataStatus 数据面（Rust memflow 后端）");
        return {};
    }

    const SharedDataSnapshot snap = read_shared_data_snapshot();
    if (snap.sync_flag != 1) {
        BOT_WARN("GameMemory", "SharedDataStatus 未就绪，sync_flag=" << snap.sync_flag);
        return {};
    }

    // 使用 role 偏移量 (0x208) 的原始值进行换算（与原项目 HV 读取逻辑一致）
    GameCoord result{};
    result.x = (snap.role_raw_x - 10) / 20;

    auto yBaseIter = kMapYBase.find(currentMapName);
    if (yBaseIter != kMapYBase.end()) {
        result.y = (yBaseIter->second - snap.role_raw_y) / 20;
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
    (void)module_name;
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

    if (!reader_->uses_shared_data_feed()) {
        if (error) {
            *error = "legacy direct memory readers are disabled; use vsock shared-data backend";
        }
        return false;
    }

    // Rust memflow 方案中，坐标数据由 Host 直接写入共享结构体，不再需要模块基址。
    dllBaseAddrs.assign(processIds.size(), 0);
    BOT_LOG("GameMemory", "使用 SharedDataStatus 数据面（Rust memflow），跳过模块基址查询");
    return true;
}
