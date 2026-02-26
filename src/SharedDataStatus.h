#pragma once

#include <cstdint>

// Host -> Guest 的物理内存共享收件箱。
// 控制面通过 vsock 只做一次 INIT_BIND，数据面由 Host 持续物理覆盖此结构体。
#pragma pack(push, 1)
struct SharedDataStatus {
    volatile std::uint32_t sync_flag = 0;  // 0=未就绪, 1=Host 正在写入, 2=请求停止
    volatile std::uint64_t timestamp = 0;  // Host 最后一次写入时间戳（微秒）
    volatile std::int32_t current_x = 0;   // 坐标 X（Host 注入）
    volatile std::int32_t current_y = 0;   // 坐标 Y（Host 注入）
    volatile std::int32_t map_id = 0;      // 地图 ID（Host 注入）
};
#pragma pack(pop)

static_assert(sizeof(SharedDataStatus) == 24, "SharedDataStatus layout mismatch");

extern SharedDataStatus g_shared_data;

struct SharedDataSnapshot {
    std::uint32_t sync_flag = 0;
    std::uint64_t timestamp = 0;
    std::int32_t current_x = 0;
    std::int32_t current_y = 0;
    std::int32_t map_id = 0;
};

SharedDataSnapshot read_shared_data_snapshot();
bool is_shared_data_ready();

