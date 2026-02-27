#pragma once

#include <cstdint>

// Host -> Guest 的物理内存共享收件箱。
// 控制面通过 vsock 只做一次 INIT_BIND，数据面由 Host 持续物理覆盖此结构体。
#pragma pack(push, 1)
struct SharedDataStatus {
    volatile std::uint32_t sync_flag = 0;  // 0=未就绪, 1=Host 正在写入, 2=请求停止
    volatile std::uint64_t timestamp = 0;  // Host 最后一次写入时间戳（微秒）
    volatile std::int32_t pit_x = 0;       // 坑位像素 X (offset 0x118)
    volatile std::int32_t pit_y = 0;       // 坑位像素 Y (offset 0x118+4)
    volatile std::int32_t map_id = 0;      // 地图 ID
    volatile std::int32_t role_raw_x = 0;  // 游戏世界原始 X (offset 0x208)
    volatile std::int32_t role_raw_y = 0;  // 游戏世界原始 Y (offset 0x208+4)
};
#pragma pack(pop)

static_assert(sizeof(SharedDataStatus) == 32, "SharedDataStatus layout mismatch");

extern SharedDataStatus g_shared_data;

struct SharedDataSnapshot {
    std::uint32_t sync_flag = 0;
    std::uint64_t timestamp = 0;
    std::int32_t pit_x = 0;
    std::int32_t pit_y = 0;
    std::int32_t map_id = 0;
    std::int32_t role_raw_x = 0;
    std::int32_t role_raw_y = 0;
};

SharedDataSnapshot read_shared_data_snapshot();
bool is_shared_data_ready();

